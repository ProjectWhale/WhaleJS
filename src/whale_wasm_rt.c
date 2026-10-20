/*
 * Runtime helpers for the wasm -> C translator.
 *
 * The translated C keeps wasm's numeric semantics, and a few of them have no C
 * expression: signed division traps on INT_MIN/-1 and on division by zero,
 * shifts mask their count, float min/max propagate the sign of zero the way the
 * spec says, and every memory access checks bounds. Those become calls into
 * this file, which the engine registers as host symbols - so the generated code
 * needs no runtime archive and no CRT.
 *
 * Kept out of whale_wasm.c on purpose: that file binds JS objects, this one is
 * pure arithmetic, and a bug in one is not a bug in the other.
 */
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* interrupt polling for translated loops                              */
/* ------------------------------------------------------------------ */
/* Translated code runs outside the interpreter, so nothing in it would notice
 * JS_SetInterruptHandler: a `loop { br 0 }` would spin forever. Every loop back
 * edge calls this, and the binding points it at JS_WhalePollInterrupt. Returns
 * non-zero when execution must stop. */
static int (*whale_interrupt_probe)(void *opaque);
static void *whale_interrupt_opaque;

void whale_wasm_set_interrupt_probe(int (*probe)(void *), void *opaque)
{
    whale_interrupt_probe = probe;
    whale_interrupt_opaque = opaque;
}

/* Translated loops poll the embedder's interrupt handler, but a call per back
 * edge costs more than the loop body in a tight loop - measured at 259ms vs
 * 202ms for wasm3 on a 1MB byte-sum. The check is amortised instead: a counter
 * decides when to call out, so the common iteration pays one decrement and one
 * branch, and the call itself happens once every WHALE_WASM_POLL_EVERY back
 * edges. The engine's own interpreter polls every 10000 bytecodes, so this is
 * the same order of magnitude.
 *
 * The counter is a global rather than a local because the translated code is
 * generated C with no place to keep it: a `static` inside a function body
 * would need a declaration the translator does not emit. */
#define WHALE_WASM_POLL_EVERY 4096

static unsigned whale_poll_countdown = WHALE_WASM_POLL_EVERY;

int whale_wasm_check_interrupt(void)
{
    if (--whale_poll_countdown != 0)
        return 0;
    whale_poll_countdown = WHALE_WASM_POLL_EVERY;
    if (whale_interrupt_probe)
        return whale_interrupt_probe(whale_interrupt_opaque);
    return 0;
}

/* ------------------------------------------------------------------ */
/* traps                                                               */
/* ------------------------------------------------------------------ */
/* Set while a translated function runs. The translated code returns 0 through
 * this on an out-of-bounds access; the caller checks the flag and raises the
 * JS exception, so a trap never has to unwind through generated code. */
static int whale_wasm_trap_flag;

/* True when the last translated call was stopped by an interrupt rather than a
 * real trap, so the binding can report the interrupt error instead of a trap. */
static int whale_interrupt_flag;

int whale_wasm_trap(void)
{
    whale_wasm_trap_flag = 1;
    return 0;
}

void whale_wasm_note_interrupt(void)
{
    whale_interrupt_flag = 1;
    whale_wasm_trap_flag = 1;   /* reuse the trap path to unwind the call */
}

int whale_wasm_interrupted(void)
{
    int f = whale_interrupt_flag;

    whale_interrupt_flag = 0;
    return f;
}

int whale_wasm_trapped(void)
{
    return whale_wasm_trap_flag;
}

void whale_wasm_clear_trap(void)
{
    whale_wasm_trap_flag = 0;
    whale_interrupt_flag = 0;
}

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */
/* The translated code keeps a module's globals in a `unsigned long long[]`
 * owned by the instance. Integer globals go in and out with a cast; a float's
 * bits have to travel unchanged, so those go through a memcpy - a cast would
 * convert the value instead of moving the representation. */
float whale_wasm_gget32(const unsigned long long *g, unsigned int i)
{
    uint32_t u = (uint32_t)g[i];
    float f;

    memcpy(&f, &u, 4);
    return f;
}

void whale_wasm_gset32(unsigned long long *g, unsigned int i, float v)
{
    uint32_t u;

    memcpy(&u, &v, 4);
    g[i] = u;
}

double whale_wasm_gget64(const unsigned long long *g, unsigned int i)
{
    double d;

    memcpy(&d, &g[i], 8);
    return d;
}

void whale_wasm_gset64(unsigned long long *g, unsigned int i, double v)
{
    memcpy(&g[i], &v, 8);
}

/* ------------------------------------------------------------------ */
/* entry wrappers: slot <-> value                                      */
/* ------------------------------------------------------------------ */
/* Every translated function also gets a wrapper taking one slot array in and
 * one out, so the binding can call any signature through a single function
 * pointer type. A float travels as its bits, not as a converted integer - the
 * slot is storage, and a cast here would turn 21.5 into 21. */
float whale_wasm_f32_from_slot(unsigned long long v)
{
    uint32_t u = (uint32_t)v;
    float f;

    memcpy(&f, &u, 4);
    return f;
}

unsigned long long whale_wasm_f32_to_slot(float f)
{
    uint32_t u;

    memcpy(&u, &f, 4);
    return (unsigned long long)u;
}

double whale_wasm_f64_from_slot(unsigned long long v)
{
    double d;

    memcpy(&d, &v, 8);
    return d;
}

unsigned long long whale_wasm_f64_to_slot(double d)
{
    unsigned long long v;

    memcpy(&v, &d, 8);
    return v;
}

/* ------------------------------------------------------------------ */
/* conversions                                                         */
/* ------------------------------------------------------------------ */
/* Float -> integer. wasm traps on NaN and on anything outside the target's
 * range; C says the behaviour is undefined there, so the check has to happen
 * before the cast. On a trap the value 0 is returned and the flag is set, the
 * same way an out-of-bounds access reports itself - the generated code has no
 * way to unwind. */
static int32_t trunc32_s(double x)
{
    if (x != x || x >= 2147483648.0 || x < -2147483648.0) {
        whale_wasm_trap();
        return 0;
    }
    return (int32_t)x;
}

static uint32_t trunc32_u(double x)
{
    if (x != x || x >= 4294967296.0 || x <= -1.0) {
        whale_wasm_trap();
        return 0;
    }
    return (uint32_t)x;
}

static int64_t trunc64_s(double x)
{
    if (x != x || x >= 9223372036854775808.0 || x < -9223372036854775808.0) {
        whale_wasm_trap();
        return 0;
    }
    return (int64_t)x;
}

static uint64_t trunc64_u(double x)
{
    if (x != x || x >= 18446744073709551616.0 || x <= -1.0) {
        whale_wasm_trap();
        return 0;
    }
    return (uint64_t)x;
}

/* Saturating variants: clamp instead of trapping, and NaN becomes 0. These are
 * what clang emits for `~~x` style float->int in many cases, so they show up
 * even in modules that never mention the saturating proposal by name. */
static int32_t trunc_sat32_s(double x)
{
    if (x != x)
        return 0;
    if (x >= 2147483647.0)
        return 2147483647;
    if (x <= -2147483648.0)
        return -2147483648;
    return (int32_t)x;
}

static uint32_t trunc_sat32_u(double x)
{
    if (x != x || x <= 0.0)
        return 0;
    if (x >= 4294967295.0)
        return 4294967295u;
    return (uint32_t)x;
}

static int64_t trunc_sat64_s(double x)
{
    if (x != x)
        return 0;
    if (x >= 9223372036854775807.0)
        return 9223372036854775807LL;
    if (x <= -9223372036854775808.0)
        return (-9223372036854775807LL - 1);
    return (int64_t)x;
}

static uint64_t trunc_sat64_u(double x)
{
    if (x != x || x <= 0.0)
        return 0;
    if (x >= 18446744073709551615.0)
        return 18446744073709551615ULL;
    return (uint64_t)x;
}

/* The exported spellings take a double in every case: an f32 argument promotes
 * to it exactly, so one function serves both source types. */
int whale_trunc32_s(double x) { return (int)trunc32_s(x); }
int whale_trunc32_u(double x) { return (int)trunc32_u(x); }
unsigned long long whale_trunc64_s(double x) { return (unsigned long long)trunc64_s(x); }
unsigned long long whale_trunc64_u(double x) { return (unsigned long long)trunc64_u(x); }

int whale_trunc_sat32_s(double x) { return (int)trunc_sat32_s(x); }
int whale_trunc_sat32_u(double x) { return (int)trunc_sat32_u(x); }
unsigned long long whale_trunc_sat64_s(double x) { return (unsigned long long)trunc_sat64_s(x); }
unsigned long long whale_trunc_sat64_u(double x) { return (unsigned long long)trunc_sat64_u(x); }

/* u64 -> float: a plain cast rounds toward zero in some compilers and is
 * simply wrong for values above 2^63, so this splits the value. */
float whale_u64_to_f32(unsigned long long v)
{
    if (v < 0x8000000000000000ULL)
        return (float)v;
    return (float)(v >> 1) * 2.0f;
}

double whale_u64_to_f64(unsigned long long v)
{
    if (v < 0x8000000000000000ULL)
        return (double)v;
    return (double)(v >> 1) * 2.0;
}

/* Bit reinterpretation, so no conversion happens. */
unsigned int whale_f32_bits(float f)
{
    unsigned int u;

    memcpy(&u, &f, 4);
    return u;
}

unsigned long long whale_f64_bits(double d)
{
    unsigned long long v;

    memcpy(&v, &d, 8);
    return v;
}

float whale_bits_f32(unsigned int u)
{
    float f;

    memcpy(&f, &u, 4);
    return f;
}

double whale_bits_f64(unsigned long long v)
{
    double d;

    memcpy(&d, &v, 8);
    return d;
}

/* ------------------------------------------------------------------ */
/* integer division: wasm traps where C is undefined                   */
/* ------------------------------------------------------------------ */
static int32_t sdiv32_trap;

int32_t whale_sdiv32(uint32_t a, uint32_t b)
{
    int32_t x = (int32_t)a, y = (int32_t)b;

    if (y == 0 || (x == INT32_MIN && y == -1)) {
        sdiv32_trap = 1;
        return 0;
    }
    return x / y;
}

int32_t whale_srem32(uint32_t a, uint32_t b)
{
    int32_t x = (int32_t)a, y = (int32_t)b;

    if (y == 0 || (x == INT32_MIN && y == -1)) {
        sdiv32_trap = 1;
        return 0;
    }
    return x % y;
}

uint64_t whale_sdiv64(uint64_t a, uint64_t b)
{
    int64_t x = (int64_t)a, y = (int64_t)b;

    if (y == 0 || (x == INT64_MIN && y == -1)) {
        sdiv32_trap = 1;
        return 0;
    }
    return (uint64_t)(x / y);
}

uint64_t whale_srem64(uint64_t a, uint64_t b)
{
    int64_t x = (int64_t)a, y = (int64_t)b;

    if (y == 0 || (x == INT64_MIN && y == -1)) {
        sdiv32_trap = 1;
        return 0;
    }
    return (uint64_t)(x % y);
}

int whale_div_trapped(void)
{
    int t = sdiv32_trap;

    sdiv32_trap = 0;
    return t;
}

/* ------------------------------------------------------------------ */
/* shifts: wasm masks the count, C shifts are undefined past the width */
/* ------------------------------------------------------------------ */
uint32_t whale_shl32(uint32_t a, uint32_t b)  { return a << (b & 31); }
uint32_t whale_shr32(uint32_t a, uint32_t b)  { return a >> (b & 31); }
uint32_t whale_shrs32(uint32_t a, uint32_t b) { return (uint32_t)((int32_t)a >> (b & 31)); }
uint64_t whale_shl64(uint64_t a, uint64_t b)  { return a << (b & 63); }
uint64_t whale_shr64(uint64_t a, uint64_t b)  { return a >> (b & 63); }
uint64_t whale_shrs64(uint64_t a, uint64_t b) { return (uint64_t)((int64_t)a >> (b & 63)); }

uint32_t whale_rotl32(uint32_t a, uint32_t b)
{
    b &= 31;
    return (a << b) | (a >> ((32 - b) & 31));
}

uint32_t whale_rotr32(uint32_t a, uint32_t b)
{
    b &= 31;
    return (a >> b) | (a << ((32 - b) & 31));
}

uint64_t whale_rotl64(uint64_t a, uint64_t b)
{
    b &= 63;
    return (a << b) | (a >> ((64 - b) & 63));
}

uint64_t whale_rotr64(uint64_t a, uint64_t b)
{
    b &= 63;
    return (a >> b) | (a << ((64 - b) & 63));
}

/* ------------------------------------------------------------------ */
/* comparisons on 64-bit values                                        */
/* ------------------------------------------------------------------ */
/* The i32 comparisons that C cannot express directly (signed vs unsigned of
   the same bit pattern) go through these too, so the generated code has one
   spelling for each relation. */
int whale_lts32(uint32_t a, uint32_t b) { return (int32_t)a <  (int32_t)b; }
uint32_t whale_lt32(uint32_t a, uint32_t b) { return a <  b; }
int whale_gts32(uint32_t a, uint32_t b) { return (int32_t)a >  (int32_t)b; }
uint32_t whale_gt32(uint32_t a, uint32_t b) { return a >  b; }
int whale_les32(uint32_t a, uint32_t b) { return (int32_t)a <= (int32_t)b; }
uint32_t whale_le32(uint32_t a, uint32_t b) { return a <= b; }
int whale_ges32(uint32_t a, uint32_t b) { return (int32_t)a >= (int32_t)b; }
uint32_t whale_ge32(uint32_t a, uint32_t b) { return a >= b; }

int whale_lts64(uint64_t a, uint64_t b) { return (int64_t)a <  (int64_t)b; }
uint32_t whale_lt64(uint64_t a, uint64_t b) { return a <  b; }
int whale_gts64(uint64_t a, uint64_t b) { return (int64_t)a >  (int64_t)b; }
uint32_t whale_gt64(uint64_t a, uint64_t b) { return a >  b; }
int whale_les64(uint64_t a, uint64_t b) { return (int64_t)a <= (int64_t)b; }
uint32_t whale_le64(uint64_t a, uint64_t b) { return a <= b; }
int whale_ges64(uint64_t a, uint64_t b) { return (int64_t)a >= (int64_t)b; }
uint32_t whale_ge64(uint64_t a, uint64_t b) { return a >= b; }

/* ------------------------------------------------------------------ */
/* float min/max/copysign, matching the spec's NaN and zero rules      */
/* ------------------------------------------------------------------ */
/* wasm's min/max: if either is NaN the result is NaN; -0 is less than +0. C's
   fmin/fmax differ on both points. */
double whale_min(double a, double b)
{
    if (a != a || b != b)
        return a + b;               /* NaN */
    if (a == b)
        return (a == 0.0 && (1.0 / a < 0.0)) ? a : b;
    return a < b ? a : b;
}

double whale_max(double a, double b)
{
    if (a != a || b != b)
        return a + b;
    if (a == b)
        return (a == 0.0 && (1.0 / a > 0.0)) ? a : b;
    return a > b ? a : b;
}

double whale_copysign(double a, double b)
{
    uint64_t ua, ub;

    memcpy(&ua, &a, 8);
    memcpy(&ub, &b, 8);
    ua = (ua & 0x7fffffffffffffffull) | (ub & 0x8000000000000000ull);
    memcpy(&a, &ua, 8);
    return a;
}

float whale_minf(float a, float b)
{
    if (a != a || b != b)
        return a + b;
    if (a == b)
        return (a == 0.0f && (1.0f / a < 0.0f)) ? a : b;
    return a < b ? a : b;
}

float whale_maxf(float a, float b)
{
    if (a != a || b != b)
        return a + b;
    if (a == b)
        return (a == 0.0f && (1.0f / a > 0.0f)) ? a : b;
    return a > b ? a : b;
}

float whale_copysignf(float a, float b)
{
    uint32_t ua, ub;

    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    ua = (ua & 0x7fffffffu) | (ub & 0x80000000u);
    memcpy(&a, &ua, 4);
    return a;
}
