/*
 * wasm -> C translator.
 *
 * Why this exists: wasm3 is a threaded interpreter - it turns wasm into a
 * sequence of op_* function pointers and runs an indirect-call chain. On this
 * project's perftest shapes, the same loops compiled by the engine's own
 * TinyCC run 1.5-3x faster than wasm3 executes them, so the loss is in the op
 * dispatch, not in the backend. This removes wasm3 from the execution path:
 * module bytes in, a C function out, compiled by the engine's one TCCState.
 *
 * Scope: MVP. An opcode it cannot translate makes the whole function fail, and
 * the caller falls back to wasm3 - correctness never depends on this file
 * keeping up with the spec.
 *
 * Two passes
 *
 * C requires declarations before statements, but the number of temporaries is
 * only known after translating the body. So the body is translated twice: the
 * first pass counts temporaries and validates, the second emits. Translation is
 * deterministic, so the two passes agree on every temporary's type and index -
 * the first pass simply does not write anything.
 *
 * Numeric model
 *
 *   i32 -> uint32_t, i64 -> uint64_t, f32 -> float, f64 -> double
 *
 * Unsigned is the storage type because wasm's integer arithmetic is modular and
 * most operators are defined on the bit pattern. Signed variants of div/rem and
 * the signed comparisons go through helpers or casts, which is where the
 * distinction actually matters.
 *
 * Control flow
 *
 * block/loop/if map onto C braces, `for(;;)` and `if`. `br` to an outer block
 * has no C expression, so each block gets a label and branches become `goto`.
 * A block with a result gets a destination slot at entry, so a forward branch
 * has somewhere to write the value it carries.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "whalejs.h"

/* Runtime helpers, defined in whale_wasm_rt.c. Declared here with their real
   signatures so the registration below can take their addresses. */
extern int whale_wasm_trap(void);
extern int whale_wasm_check_interrupt(void);
extern void whale_wasm_note_interrupt(void);
extern float whale_wasm_gget32(const unsigned long long *, unsigned int);
extern void whale_wasm_gset32(unsigned long long *, unsigned int, float);
extern double whale_wasm_gget64(const unsigned long long *, unsigned int);
extern void whale_wasm_gset64(unsigned long long *, unsigned int, double);
extern float whale_wasm_f32_from_slot(unsigned long long);
extern unsigned long long whale_wasm_f32_to_slot(float);
extern double whale_wasm_f64_from_slot(unsigned long long);
extern unsigned long long whale_wasm_f64_to_slot(double);
extern int whale_trunc32_s(double);
extern int whale_trunc32_u(double);
extern unsigned long long whale_trunc64_s(double);
extern unsigned long long whale_trunc64_u(double);
extern int whale_trunc_sat32_s(double);
extern int whale_trunc_sat32_u(double);
extern unsigned long long whale_trunc_sat64_s(double);
extern unsigned long long whale_trunc_sat64_u(double);
extern float whale_u64_to_f32(unsigned long long);
extern double whale_u64_to_f64(unsigned long long);
extern unsigned int whale_f32_bits(float);
extern unsigned long long whale_f64_bits(double);
extern float whale_bits_f32(unsigned int);
extern double whale_bits_f64(unsigned long long);
extern int whale_wasm_trapped(void);
extern int whale_div_trapped(void);
extern int whale_sdiv32(unsigned int, unsigned int);
extern int whale_srem32(unsigned int, unsigned int);
extern unsigned long long whale_sdiv64(unsigned long long, unsigned long long);
extern unsigned long long whale_srem64(unsigned long long, unsigned long long);
extern unsigned int whale_shl32(unsigned int, unsigned int);
extern unsigned int whale_shr32(unsigned int, unsigned int);
extern unsigned int whale_shrs32(unsigned int, unsigned int);
extern unsigned long long whale_shl64(unsigned long long, unsigned long long);
extern unsigned long long whale_shr64(unsigned long long, unsigned long long);
extern unsigned long long whale_shrs64(unsigned long long, unsigned long long);
extern unsigned int whale_rotl32(unsigned int, unsigned int);
extern unsigned int whale_rotr32(unsigned int, unsigned int);
extern unsigned long long whale_rotl64(unsigned long long, unsigned long long);
extern unsigned long long whale_rotr64(unsigned long long, unsigned long long);
extern int whale_lts32(unsigned int, unsigned int);
extern unsigned int whale_lt32(unsigned int, unsigned int);
extern int whale_gts32(unsigned int, unsigned int);
extern unsigned int whale_gt32(unsigned int, unsigned int);
extern int whale_les32(unsigned int, unsigned int);
extern unsigned int whale_le32(unsigned int, unsigned int);
extern int whale_ges32(unsigned int, unsigned int);
extern unsigned int whale_ge32(unsigned int, unsigned int);
extern int whale_lts64(unsigned long long, unsigned long long);
extern unsigned int whale_lt64(unsigned long long, unsigned long long);
extern int whale_gts64(unsigned long long, unsigned long long);
extern unsigned int whale_gt64(unsigned long long, unsigned long long);
extern int whale_les64(unsigned long long, unsigned long long);
extern unsigned int whale_le64(unsigned long long, unsigned long long);
extern int whale_ges64(unsigned long long, unsigned long long);
extern unsigned int whale_ge64(unsigned long long, unsigned long long);
extern double whale_min(double, double);
extern double whale_max(double, double);
extern double whale_copysign(double, double);
extern float whale_minf(float, float);
extern float whale_maxf(float, float);
extern float whale_copysignf(float, float);

static int whale_helpers_registered;

/* Diagnostic switch, looked up once: it only enables the per-function tracing
 * used while working on the translator. */
static int whale_wasm_c_dbg(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("WHALE_WASM_DBG") != NULL;
    return on;
}

/* Token emission, provided by the engine (src/tcc/whale_tcc_iface.c). */
extern void JS_WhaleTokReset(void);
extern void JS_WhaleTokPush(int t, double val);
extern void JS_WhaleTokIdent2(const char *name);
extern void JS_WhaleTokChar(int ch);
extern void JS_WhaleTokInt(int v);
extern void JS_WhaleTokDouble(double v);
extern void JS_WhaleTokOp(int o);
extern void JS_WhaleTokKw(int w);

/* JS_WhaleTokKw indices, matching the table in whale_tcc_iface.c. */
enum {
    WK_INT = 0, WK_CHAR, WK_VOID, WK_LONG, WK_UNSIGNED, WK_FLOAT, WK_DOUBLE,
    WK_STATIC, WK_EXTERN, WK_TYPEDEF, WK_STRUCT, WK_RETURN, WK_IF, WK_ELSE,
    WK_WHILE, WK_FOR, WK_DO, WK_BREAK, WK_CONTINUE, WK_GOTO, WK_SWITCH,
    WK_CASE, WK_DEFAULT, WK_SIZEOF, WK_REGISTER, WK_UNION, WK_ENUM, WK_SHORT
};

/* JS_WhaleTokOp indices. */
enum {
    OP_ADD = 0, OP_SUB, OP_MUL, OP_DIV, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_EQ, OP_NE, OP_LOR, OP_LAND, OP_SHL, OP_SAR, OP_SHR, OP_ARROW, OP_DOT
};

static void id(const char *s) { JS_WhaleTokIdent2(s); }
static void ch(int c) { JS_WhaleTokChar(c); }
static void kw(int w) { JS_WhaleTokKw(w); }
static void op(int o) { JS_WhaleTokOp(o); }
static void inum(int v) { JS_WhaleTokInt(v); }
static void dnum(double v) { JS_WhaleTokDouble(v); }

typedef enum { T_NONE = 0, T_I32, T_I64, T_F32, T_F64 } WType;

#define MAX_LOCALS 256
#define MAX_REGS 1024
#define MAX_DEPTH 512
#define MAX_CTRL 128

/* ---- module-level context -------------------------------------------
 * A call from one translated function to another is a direct C call, so the
 * translator has to know every function's signature and whether it is going to
 * be translated. Both live here, filled by the binding before translation.
 */
#define WASM_C_MAX_FUNCS   512
#define WASM_C_MAX_GLOBALS 128

typedef struct WasmCFuncSig {
    int nparams;
    int params[8];
    int nresults;
    int results[1];
} WasmCFuncSig;

typedef struct WasmCModule {
    int nfuncs;                                 /* the function index space */
    WasmCFuncSig sig[WASM_C_MAX_FUNCS];
    unsigned char translated[WASM_C_MAX_FUNCS];
    char sym[WASM_C_MAX_FUNCS][40];
    int nglobals;
    int global_type[WASM_C_MAX_GLOBALS];        /* 1..4, or 0 = unsupported */
} WasmCModule;

typedef struct FnCtx {
    const uint8_t *body;
    const uint8_t *end;
    int ok;
    int quiet;                  /* pass 1: count only, emit nothing */
    char err[192];

    WasmCModule *mod;
    int func_index;
    /* Makes this function's C labels unique. The translator compiles every
       function of a module into the engine's one TCCState, and the labels all
       used to be `Lb0`, `Lb1`, ... - so the second translated function with a
       loop collided with the first ("duplicate label 'Lb1'"). Set from the
       symbol name, which the binding already makes unique per module+function.
       Sized for the longest such name (`whale_wasm_<u32>_<u32>`) so the copy
       cannot truncate into a collision. */
    char label_scope[48];
    /* The functions this body calls, so the emitted unit declares exactly
       those and not the whole module. */
    unsigned char callees[WASM_C_MAX_FUNCS];

    int nparams, nresults;
    WType params[32];
    WType results[4];

    int nlocals;
    WType local_type[MAX_LOCALS];
    char local_name[MAX_LOCALS][16];

    int sp;
    WType stk_type[MAX_DEPTH];
    int stk_reg[MAX_DEPTH];

    int nregs;
    WType reg_type[MAX_REGS];

    int depth;
    struct Ctrl {
        int kind;               /* 0 block, 1 loop, 2 if, 3 function */
        int label;
        int sp_at_entry;
        int has_result;
        WType result;
        int result_reg;
        int back_edge;          /* loop: the label is already at the top */
    } ctrl[MAX_CTRL];

    int next_label;
    uint8_t local_types[256];   /* raw, re-read each pass */
    int local_blocks;
} FnCtx;

static void fail(FnCtx *f, const char *msg)
{
    if (f->ok) {
        f->ok = 0;
        snprintf(f->err, sizeof(f->err), "%s", msg);
    }
}

/* ---- decoding ------------------------------------------------------- */
static uint32_t rd_u32(FnCtx *f)
{
    uint32_t v = 0;
    int shift = 0;

    while (f->body < f->end) {
        uint8_t b = *f->body++;

        v |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
        shift += 7;
        if (shift > 35) {
            fail(f, "leb overflow");
            return 0;
        }
    }
    fail(f, "underrun");
    return 0;
}

static int64_t rd_s64(FnCtx *f)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b;

    do {
        if (f->body >= f->end) {
            fail(f, "underrun");
            return 0;
        }
        b = *f->body++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (shift > 70) {
            fail(f, "leb overflow");
            return 0;
        }
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}

static uint8_t rd_u8(FnCtx *f)
{
    if (f->body >= f->end) {
        fail(f, "underrun");
        return 0;
    }
    return *f->body++;
}

static double rd_f32(FnCtx *f)
{
    uint32_t u = 0;
    float fl;
    int i;

    for (i = 0; i < 4; i++)
        u |= (uint32_t)rd_u8(f) << (8 * i);
    memcpy(&fl, &u, 4);
    return (double)fl;
}

static double rd_f64(FnCtx *f)
{
    uint64_t u = 0;
    double d;
    int i;

    for (i = 0; i < 8; i++)
        u |= (uint64_t)rd_u8(f) << (8 * i);
    memcpy(&d, &u, 8);
    return d;
}

/* ---- emit helpers --------------------------------------------------- */
#define E(f, ...) do { if (!(f)->quiet) f->emit_ops++; } while (0)

/* The generated C is fed to TinyCC's parser as tokens, and only the built-in
   type keywords are keywords there - `uint32_t` and friends are typedef names
   the parser has never seen, because no header is read. So the types are
   spelled with the built-in keywords, widened the way wasm needs:
 *
 *   i32 -> unsigned int      i64 -> unsigned long long
 *   f32 -> float             f64 -> double
 *
 * The translator assumes 32-bit int and 64-bit long long, which is what the
 * TCC x86-64 backend and every platform this engine builds for use. */
static const char *ctype(WType t)
{
    switch (t) {
    case T_I32: return "unsigned int";
    case T_I64: return "unsigned long long";
    case T_F32: return "float";
    case T_F64: return "double";
    default:    return "unsigned int";
    }
}

/* Emit a type name: each word is its own identifier token, so `unsigned long
   long` must be pushed as three. */
static void put_type_kw(int t)
{
    switch (t) {
    case T_I32: kw(WK_UNSIGNED); kw(WK_INT); break;
    case T_I64: kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG); break;
    case T_F32: kw(WK_FLOAT); break;
    case T_F64: kw(WK_DOUBLE); break;
    default:    kw(WK_UNSIGNED); kw(WK_INT); break;
    }
}

static void put_type(FnCtx *f, WType t)
{
    if (f->quiet)
        return;
    put_type_kw((int)t);
}

static int newreg(FnCtx *f, WType t)
{
    int r;

    if (f->nregs >= MAX_REGS) {
        fail(f, "too many temporaries");
        return 0;
    }
    r = f->nregs++;
    f->reg_type[r] = t;
    return r;
}

static int pushreg(FnCtx *f, WType t)
{
    int r = newreg(f, t);

    if (!f->ok)
        return 0;
    if (f->sp >= MAX_DEPTH) {
        fail(f, "value stack overflow");
        return 0;
    }
    f->stk_type[f->sp] = t;
    f->stk_reg[f->sp] = r;
    f->sp++;
    return r;
}

static int popreg(FnCtx *f, WType expect)
{
    if (f->sp <= 0) {
        fail(f, "value stack underflow");
        return 0;
    }
    f->sp--;
    if (f->stk_type[f->sp] != expect) {
        fail(f, "type mismatch");
        return 0;
    }
    return f->stk_reg[f->sp];
}

static void put_reg(FnCtx *f, int r)
{
    char nm[16];

    if (f->quiet)
        return;
    snprintf(nm, sizeof(nm), "v%d", r);
    id(nm);
}

/* `vR = ` */
static void put_assign(FnCtx *f, int r)
{
    if (f->quiet)
        return;
    put_reg(f, r);
    ch('=');
}

static void put_cstr(FnCtx *f, const char *s)
{
    if (!f->quiet)
        id(s);
}

static void put_ch(FnCtx *f, int c)
{
    if (!f->quiet)
        ch(c);
}

static void put_semi(FnCtx *f)
{
    if (!f->quiet)
        ch(';');
}

static void put_label(FnCtx *f, const char *prefix, int label)
{
    char nm[80];

    if (f->quiet)
        return;
    /* The prefix carries the translated function's own symbol, so two functions
       translated into the same C unit cannot collide. C labels are scoped to
       the function, so this is belt-and-braces - but TinyCC's parser reported
       "duplicate label" for labels in *different* functions of the same unit
       once (the token stream is one unit, and the label stack is checked before
       the function scope is entered), so the uniqueness is load-bearing. */
    snprintf(nm, sizeof(nm), "%s_%s%d", prefix, f->label_scope, label);
    id(nm);
    ch(':');
    /* A label must be followed by a statement. Without this, a block that ends
       right before its enclosing `}` leaves `Lb1: }`, which TinyCC warns about
       ("deprecated use of label at end of compound statement") and which is
       not valid C in every dialect. A null statement is. */
    ch(';');
}

static void put_goto(FnCtx *f, int label)
{
    char nm[80];

    if (f->quiet)
        return;
    kw(WK_GOTO);
    snprintf(nm, sizeof(nm), "Lb_%s%d", f->label_scope, label);
    id(nm);
    ch(';');
}

/* Emit a C binary operator for the token stream. */
static void put_binop(FnCtx *f, int wasm_op)
{
    (void)f;
    (void)wasm_op;
}

/* ---- operator table -------------------------------------------------- */
typedef struct OpDef {
    int code;
    WType type;             /* operand type; result is I32 when cond */
    const char *c_op;       /* C operator, if it maps directly */
    const char *helper;     /* engine helper, otherwise */
    int cond;               /* comparison -> i32 */
} OpDef;

static const OpDef ops[] = {
    /* i32 */
    { 0x6a, T_I32, "+", NULL, 0 }, { 0x6b, T_I32, "-", NULL, 0 },
    { 0x6c, T_I32, "*", NULL, 0 }, { 0x6d, T_I32, "/", NULL, 0 },
    { 0x6e, T_I32, NULL, "whale_sdiv32", 0 },
    { 0x6f, T_I32, "%", NULL, 0 },
    { 0x70, T_I32, NULL, "whale_srem32", 0 },
    { 0x71, T_I32, "&", NULL, 0 }, { 0x72, T_I32, "|", NULL, 0 },
    { 0x73, T_I32, "^", NULL, 0 },
    { 0x74, T_I32, NULL, "whale_shl32", 0 },
    { 0x75, T_I32, NULL, "whale_shrs32", 0 },
    { 0x76, T_I32, NULL, "whale_shr32", 0 },
    { 0x77, T_I32, NULL, "whale_rotl32", 0 },
    { 0x78, T_I32, NULL, "whale_rotr32", 0 },
    /* i64 */
    { 0x7c, T_I64, "+", NULL, 0 }, { 0x7d, T_I64, "-", NULL, 0 },
    { 0x7e, T_I64, "*", NULL, 0 }, { 0x7f, T_I64, "/", NULL, 0 },
    { 0x80, T_I64, NULL, "whale_sdiv64", 0 },
    { 0x81, T_I64, "%", NULL, 0 },
    { 0x82, T_I64, NULL, "whale_srem64", 0 },
    { 0x83, T_I64, "&", NULL, 0 }, { 0x84, T_I64, "|", NULL, 0 },
    { 0x85, T_I64, "^", NULL, 0 },
    { 0x86, T_I64, NULL, "whale_shl64", 0 },
    { 0x87, T_I64, NULL, "whale_shrs64", 0 },
    { 0x88, T_I64, NULL, "whale_shr64", 0 },
    { 0x89, T_I64, NULL, "whale_rotl64", 0 },
    { 0x8a, T_I64, NULL, "whale_rotr64", 0 },
    /* f32 */
    { 0x92, T_F32, "+", NULL, 0 }, { 0x93, T_F32, "-", NULL, 0 },
    { 0x94, T_F32, "*", NULL, 0 }, { 0x95, T_F32, "/", NULL, 0 },
    { 0x96, T_F32, NULL, "whale_minf", 0 },
    { 0x97, T_F32, NULL, "whale_maxf", 0 },
    { 0x98, T_F32, NULL, "whale_copysignf", 0 },
    /* f64 */
    { 0xa0, T_F64, "+", NULL, 0 }, { 0xa1, T_F64, "-", NULL, 0 },
    { 0xa2, T_F64, "*", NULL, 0 }, { 0xa3, T_F64, "/", NULL, 0 },
    { 0xa4, T_F64, NULL, "whale_min", 0 },
    { 0xa5, T_F64, NULL, "whale_max", 0 },
    { 0xa6, T_F64, NULL, "whale_copysign", 0 },
    /* comparisons */
    { 0x46, T_I32, "eq", NULL, 1 }, { 0x47, T_I32, "ne", NULL, 1 },
    { 0x48, T_I32, NULL, "whale_lts32", 1 },
    { 0x49, T_I32, NULL, "whale_lt32", 1 },
    { 0x4a, T_I32, NULL, "whale_gts32", 1 },
    { 0x4b, T_I32, NULL, "whale_gt32", 1 },
    { 0x4c, T_I32, NULL, "whale_les32", 1 },
    { 0x4d, T_I32, NULL, "whale_le32", 1 },
    { 0x4e, T_I32, NULL, "whale_ges32", 1 },
    { 0x4f, T_I32, NULL, "whale_ge32", 1 },
    { 0x50, T_I64, "eq", NULL, 1 }, { 0x51, T_I64, "ne", NULL, 1 },
    { 0x52, T_I64, NULL, "whale_lts64", 1 },
    { 0x53, T_I64, NULL, "whale_lt64", 1 },
    { 0x54, T_I64, NULL, "whale_gts64", 1 },
    { 0x55, T_I64, NULL, "whale_gt64", 1 },
    { 0x56, T_I64, NULL, "whale_les64", 1 },
    { 0x57, T_I64, NULL, "whale_le64", 1 },
    { 0x58, T_I64, NULL, "whale_ges64", 1 },
    { 0x59, T_I64, NULL, "whale_ge64", 1 },
    { 0x5a, T_F32, "eq", NULL, 1 }, { 0x5b, T_F32, "ne", NULL, 1 },
    { 0x5c, T_F32, "lt", NULL, 1 }, { 0x5d, T_F32, "gt", NULL, 1 },
    { 0x5e, T_F32, "le", NULL, 1 }, { 0x5f, T_F32, "ge", NULL, 1 },
    { 0x61, T_F64, "eq", NULL, 1 }, { 0x62, T_F64, "ne", NULL, 1 },
    { 0x63, T_F64, "lt", NULL, 1 }, { 0x64, T_F64, "gt", NULL, 1 },
    { 0x65, T_F64, "le", NULL, 1 }, { 0x66, T_F64, "ge", NULL, 1 },
};

static const OpDef *find_op(int code)
{
    size_t i;

    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (ops[i].code == code)
            return &ops[i];
    return NULL;
}

/* Emit `(a <c-op> b)`. */
static void emit_cmp(FnCtx *f, const char *which, int a, int b)
{
    if (f->quiet)
        return;
    ch('(');
    put_reg(f, a);
    if (!strcmp(which, "eq")) op(OP_EQ);
    else if (!strcmp(which, "ne")) op(OP_NE);
    else if (!strcmp(which, "lt")) op(OP_LT);
    else if (!strcmp(which, "gt")) op(OP_GT);
    else if (!strcmp(which, "le")) op(OP_LE);
    else op(OP_GE);
    put_reg(f, b);
    ch(')');
}

static void emit_bin(FnCtx *f, const char *c_op, int a, int b)
{
    if (f->quiet)
        return;
    ch('(');
    put_reg(f, a);
    if (!strcmp(c_op, "+")) op(OP_ADD);
    else if (!strcmp(c_op, "-")) op(OP_SUB);
    else if (!strcmp(c_op, "*")) op(OP_MUL);
    else if (!strcmp(c_op, "/")) op(OP_DIV);
    else if (!strcmp(c_op, "&")) ch('&');
    else if (!strcmp(c_op, "|")) ch('|');
    else if (!strcmp(c_op, "^")) ch('^');
    put_reg(f, b);
    ch(')');
}

static void emit_call2(FnCtx *f, const char *fn, int a, int b)
{
    if (f->quiet)
        return;
    id(fn);
    ch('(');
    put_reg(f, a);
    ch(',');
    put_reg(f, b);
    ch(')');
}

/* ---- control -------------------------------------------------------- */
static int ctrl_push(FnCtx *f, int kind, int has_result, WType rt)
{
    struct Ctrl *c;

    if (f->depth >= MAX_CTRL) {
        fail(f, "control nesting too deep");
        return 0;
    }
    c = &f->ctrl[f->depth++];
    c->kind = kind;
    c->sp_at_entry = f->sp;
    c->has_result = has_result;
    c->result = rt;
    c->result_reg = has_result ? newreg(f, rt) : -1;
    c->label = f->next_label++;
    c->back_edge = 0;
    return 1;
}

/* `br` out of the function: the outermost control frame is the body itself,
 * and its `end` emits a `return` rather than a label. A `goto` to it would
 * name a label nothing defines - the parse then runs off the end of the token
 * stream ("identifier expected" past the last token). Branching to the body
 * means "return the carried value", so emit exactly that. */
static void emit_return_value(FnCtx *f, struct Ctrl *c, int value_reg)
{
    if (f->quiet)
        return;
    kw(WK_RETURN);
    if (c->has_result && value_reg >= 0)
        put_reg(f, value_reg);
    else
        inum(0);
    ch(';');
}

static void emit_br(FnCtx *f, uint32_t depth)
{
    int target;
    struct Ctrl *c;

    if ((int)depth >= f->depth) {
        fail(f, "br depth out of range");
        return;
    }
    target = f->depth - 1 - (int)depth;
    c = &f->ctrl[target];

    if (c->has_result) {
        int r;

        if (f->sp <= 0) {
            fail(f, "br without a value");
            return;
        }
        if (f->stk_type[f->sp - 1] != c->result) {
            fail(f, "br value type mismatch");
            return;
        }
        r = f->stk_reg[f->sp - 1];
        f->sp--;
        if (c->kind == 3) {
            emit_return_value(f, c, r);
            return;
        }
        put_assign(f, c->result_reg);
        put_reg(f, r);
        put_semi(f);
    } else if (c->kind == 3) {
        emit_return_value(f, c, -1);
        return;
    }
    put_goto(f, c->label);
}

/* `br_if`: the condition is popped, and the target block's carried value is
 * moved into its result slot only on the taken path. wasm keeps that value on
 * the stack when the branch is NOT taken - it is the fallthrough value - so
 * this peeks rather than pops. emit_br() cannot be used: it pops, which is
 * correct for a plain `br` (the code after is unreachable) and wrong here.
 *
 * Both passes must run the same stack effect. They used to diverge - pass 1
 * returned early on f->quiet without touching the stack at all - and pass 2
 * then failed with "value stack underflow" on a body pass 1 had accepted. */
static void emit_br_if(FnCtx *f, uint32_t depth, int cond)
{
    int target;
    struct Ctrl *c;

    if ((int)depth >= f->depth) {
        fail(f, "br_if depth out of range");
        return;
    }
    target = f->depth - 1 - (int)depth;
    c = &f->ctrl[target];

    if (c->has_result) {
        if (f->sp <= 0) {
            fail(f, "br_if without a value");
            return;
        }
        if (f->stk_type[f->sp - 1] != c->result) {
            fail(f, "br_if value type mismatch");
            return;
        }
    }
    if (f->quiet)
        return;

    kw(WK_IF);
    ch('(');
    put_reg(f, cond);
    ch(')');
    ch('{');
    if (c->has_result) {
        if (c->kind == 3) {
            emit_return_value(f, c, f->stk_reg[f->sp - 1]);
        } else {
            put_assign(f, c->result_reg);
            put_reg(f, f->stk_reg[f->sp - 1]);
            put_semi(f);
            put_goto(f, c->label);
        }
    } else if (c->kind == 3) {
        emit_return_value(f, c, -1);
    } else {
        put_goto(f, c->label);
    }
    ch('}');
}

/* A block closing: its result, if any, moves into the result slot. */
static void close_ctrl(FnCtx *f, int emit_label)
{
    struct Ctrl *c;
    int label;

    if (f->depth <= 0) {
        fail(f, "end without a block");
        return;
    }
    c = &f->ctrl[f->depth - 1];
    label = c->label;

    if (c->has_result && f->sp > c->sp_at_entry) {
        int r = f->stk_reg[f->sp - 1];

        put_assign(f, c->result_reg);
        put_reg(f, r);
        put_semi(f);
    }
    put_ch(f, '}');
    /* A loop's label is at the top of its body, so `br 0` and falling off the
       end both reach it; a block's label marks where a forward branch lands,
       which is just after the closing brace. */
    if (emit_label && !c->back_edge)
        put_label(f, "Lb", label);
    f->sp = c->sp_at_entry;
    f->depth--;
    if (c->has_result) {
        if (f->sp >= MAX_DEPTH) {
            fail(f, "value stack overflow");
            return;
        }
        f->stk_type[f->sp] = c->result;
        f->stk_reg[f->sp] = c->result_reg;
        f->sp++;
    }
}

/* ---- memory --------------------------------------------------------- */
/* A block type byte: 0x40 is "no result", the value types are the rest. */
static WType blocktype(uint8_t bt)
{
    switch (bt) {
    case 0x7f: return T_I32;
    case 0x7e: return T_I64;
    case 0x7d: return T_F32;
    case 0x7c: return T_F64;
    default:   return T_NONE;       /* 0x40, or a type index (unsupported) */
    }
}

static int width_of(int opcode)
{
    switch (opcode) {
    /* loads */
    case 0x2c: case 0x2d: case 0x30: case 0x31: return 1;
    case 0x2e: case 0x2f: case 0x32: case 0x33: return 2;
    case 0x28: case 0x2a: case 0x34: case 0x35: return 4;
    case 0x29: case 0x2b: return 8;
    /* stores - the spec's numbering, the same one store_type uses */
    case 0x36: case 0x38: return 4;             /* i32.store, f32.store */
    case 0x37: case 0x39: return 8;             /* i64.store, f64.store */
    case 0x3a: case 0x3c: return 1;             /* i32.store8, i64.store8 */
    case 0x3b: case 0x3d: return 2;             /* i32.store16, i64.store16 */
    case 0x3e: return 4;                        /* i64.store32 */
    default:   return 0;
    }
}

/* Memory accesses are spelled with the built-in keywords too (see ctype).
 * Load opcodes, by the spec's numbering:
 *   0x28 i32.load    0x29 i64.load    0x2a f32.load   0x2b f64.load
 *   0x2c i32.load8_s  0x2d i32.load8_u
 *   0x2e i32.load16_s 0x2f i32.load16_u
 *   0x30 i64.load8_s  0x31 i64.load8_u
 *   0x32 i64.load16_s 0x33 i64.load16_u
 *   0x34 i64.load32_s 0x35 i64.load32_u
 */
static const char *load_type(int opcode)
{
    switch (opcode) {
    case 0x2c: return "signed char";         /* i32.load8_s */
    case 0x2d: return "unsigned char";       /* i32.load8_u */
    case 0x2e: return "short";               /* i32.load16_s */
    case 0x2f: return "unsigned short";      /* i32.load16_u */
    case 0x28: return "unsigned int";        /* i32.load */
    case 0x30: return "signed char";         /* i64.load8_s */
    case 0x31: return "unsigned char";       /* i64.load8_u */
    case 0x32: return "short";               /* i64.load16_s */
    case 0x33: return "unsigned short";      /* i64.load16_u */
    case 0x34: return "int";                 /* i64.load32_s */
    case 0x35: return "unsigned int";        /* i64.load32_u */
    case 0x29: return "unsigned long long";  /* i64.load */
    case 0x2a: return "float";               /* f32.load */
    case 0x2b: return "double";              /* f64.load */
    default:   return NULL;
    }
}

static void put_load_type(FnCtx *f, const char *spelling)
{
    if (f->quiet)
        return;
    if (!strcmp(spelling, "unsigned char")) { kw(WK_UNSIGNED); kw(WK_CHAR); }
    else if (!strcmp(spelling, "signed char")) { kw(WK_CHAR); }
    else if (!strcmp(spelling, "unsigned short")) { kw(WK_UNSIGNED); kw(WK_SHORT); }
    else if (!strcmp(spelling, "short")) { kw(WK_SHORT); }
    else if (!strcmp(spelling, "unsigned int")) { kw(WK_UNSIGNED); kw(WK_INT); }
    else if (!strcmp(spelling, "int")) { kw(WK_INT); }
    else if (!strcmp(spelling, "unsigned long long")) {
        kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG);
    } else if (!strcmp(spelling, "float")) { kw(WK_FLOAT); }
    else if (!strcmp(spelling, "double")) { kw(WK_DOUBLE); }
}

static WType load_result(int opcode)
{
    switch (opcode) {
    case 0x28: case 0x2c: case 0x2d: case 0x2e: case 0x2f: return T_I32;
    case 0x29: case 0x30: case 0x31: case 0x32:
    case 0x33: case 0x34: case 0x35: return T_I64;
    case 0x2a: return T_F32;
    case 0x2b: return T_F64;
    default:   return T_NONE;
    }
}

/* Store opcodes, by the spec's numbering. The widths are part of the opcode:
 *   0x36 i32.store    0x37 i64.store    0x38 f32.store    0x39 f64.store
 *   0x3a i32.store8   0x3b i32.store16  0x3c i64.store8   0x3d i64.store16
 *   0x3e i64.store32
 *
 * This table used to be shifted by two (0x36 was read as i32.store8), so a
 * 4-byte `i32.store` emitted a 1-byte store and everything but the low byte was
 * lost. It stayed hidden because nothing wrote memory through a translated
 * function in the test set - es-module-lexer, the one real module that reached
 * the native path, builds its offset table that way, which is what surfaced it.
 */
static WType store_value(int opcode)
{
    switch (opcode) {
    case 0x36: case 0x3a: case 0x3b: return T_I32;   /* i32.store / store8 / store16 */
    case 0x37: case 0x3c: case 0x3d: case 0x3e: return T_I64;
    case 0x38: return T_F32;
    case 0x39: return T_F64;
    default:   return T_NONE;
    }
}

static const char *store_type(int opcode)
{
    switch (opcode) {
    case 0x36: return "unsigned int";         /* i32.store */
    case 0x37: return "unsigned long long";   /* i64.store */
    case 0x38: return "float";                /* f32.store */
    case 0x39: return "double";               /* f64.store */
    case 0x3a: return "unsigned char";        /* i32.store8 */
    case 0x3b: return "unsigned short";       /* i32.store16 */
    case 0x3c: return "unsigned char";        /* i64.store8 */
    case 0x3d: return "unsigned short";       /* i64.store16 */
    case 0x3e: return "unsigned int";         /* i64.store32 */
    default:   return NULL;
    }
}

/* Load opcodes, same numbering:
 *   0x28 i32.load  0x29 i64.load  0x2a f32.load  0x2b f64.load
 *   0x2c i32.load8_s  0x2d i32.load8_u  0x2e i32.load16_s  0x2f i32.load16_u
 *   0x30 i64.load8_s  0x31 i64.load8_u  0x32 i64.load16_s  0x33 i64.load16_u
 *   0x34 i64.load32_s 0x35 i64.load32_u
/* A loop back edge checks for an interrupt request. Without this, a translated
   `loop { br 0 }` runs forever: it is native code, so the engine's interrupt
   handler has no call site to reach. */
static void emit_loop_check(FnCtx *f)
{
    if (f->quiet)
        return;
    kw(WK_IF);
    ch('(');
    id("whale_wasm_check_interrupt");
    ch('(');
    ch(')');
    ch(')');
    ch('{');
    id("whale_wasm_note_interrupt");
    ch('(');
    ch(')');
    ch(';');
    kw(WK_RETURN);
    id("whale_wasm_trap");
    ch('(');
    ch(')');
    ch(';');
    ch('}');
}

/* Set when the last translation attempt failed only because a sibling it calls
 * is not translated yet. Only a diagnostic: the binding's fixpoint loop retries
 * everything that failed, and a body that failed for another reason just fails
 * again. */
static unsigned char g_call_blocked[WASM_C_MAX_FUNCS];

/* A direct call to another translated function in the same module.
 *
 * Every translated function takes the same trailing triple (mem, memsize,
 * glob), so the call passes the caller's own through: the callee runs against
 * the same instance. `is_return` emits `return f(...)`, which is wasm's
 * return_call and lets the C compiler turn it into a jump. */
static void emit_call_direct(FnCtx *f, uint32_t idx, int is_return)
{
    WasmCFuncSig *s;
    int args[8];
    int i, r = -1;

    if (!f->mod || idx >= (uint32_t)f->mod->nfuncs) {
        fail(f, "call index out of range");
        return;
    }
    if (f->mod->sig[idx].nparams < 0) {
        fail(f, "callee signature not representable");
        return;
    }
    if (!f->mod->translated[idx]) {
        /* The callee keeps its wasm3 body, so this caller has to as well:
           mixing the two inside one call would need a re-entry path that does
           not exist. The binding propagates this to a fixpoint. */
        g_call_blocked[f->func_index] = 1;
        fail(f, "call to a function that is not translated");
        return;
    }
    f->callees[idx] = 1;
    s = &f->mod->sig[idx];

    for (i = s->nparams - 1; i >= 0; i--)
        args[i] = popreg(f, (WType)s->params[i]);
    if (!f->ok)
        return;

    if (!is_return && s->nresults > 0) {
        r = pushreg(f, (WType)s->results[0]);
        if (!f->ok)
            return;
    }
    if (f->quiet)
        return;

    if (is_return)
        kw(WK_RETURN);
    else if (r >= 0)
        put_assign(f, r);
    id(f->mod->sym[idx]);
    ch('(');
    for (i = 0; i < s->nparams; i++) {
        if (i)
            ch(',');
        put_reg(f, args[i]);
    }
    if (s->nparams)
        ch(',');
    id("mem"); ch(','); id("memsize"); ch(','); id("glob");
    ch(')');
    ch(';');
}

/* Bounds-check an address, then trap through the host. `memsize` is a
   parameter of every translated function, so the check is one comparison and
   is safe with several instances alive at once. */
static void emit_bounds(FnCtx *f, int addr, int width)
{
    if (f->quiet)
        return;
    kw(WK_IF);
    ch('(');
    put_reg(f, addr);
    op(OP_GT);
    id("memsize");
    op(OP_SUB);
    inum(width - 1);
    ch(')');
    ch('{');
    kw(WK_RETURN);
    id("whale_wasm_trap");
    ch('(');
    ch(')');
    ch(';');
    ch('}');
}

/* vAddr = base + offset */
static void emit_addr(FnCtx *f, int slot, int base, uint32_t offset)
{
    put_assign(f, slot);
    put_reg(f, base);
    if (offset) {
        if (!f->quiet)
            op(OP_ADD);
        inum((int)offset);
    }
    put_semi(f);
}

/* ---- the translator -------------------------------------------------- */
static void translate(FnCtx *f)
{
    while (f->ok) {
        int opcode;

        if (f->body >= f->end) {
            fail(f, "body ended without end");
            return;
        }
        opcode = rd_u8(f);
        if (!f->ok)
            return;

        switch (opcode) {
        case 0x00:      /* unreachable */
            if (!f->quiet) {
                kw(WK_RETURN);
                id("whale_wasm_trap");
                ch('(');
                ch(')');
                ch(';');
            }
            break;

        case 0x01:      /* nop */
            break;

        case 0x02:      /* block */
        case 0x03: {    /* loop */
            uint8_t bt = rd_u8(f);
            WType rt = (bt == 0x7f) ? T_I32 : (bt == 0x7e) ? T_I64 :
                       (bt == 0x7d) ? T_F32 : (bt == 0x7c) ? T_F64 : T_NONE;
            int is_loop = (opcode == 0x03);

            if (!ctrl_push(f, is_loop, rt != T_NONE, rt))
                return;
            if (!f->quiet) {
                if (is_loop) {
                    /* The back edge is the `for(;;)` itself, so `br 0` jumps to
                       the top of the body and the label goes there. `back_edge`
                       remembers which label that was, because the block's own
                       `end` must not emit a second one. */
                    kw(WK_FOR);
                    ch('('); ch(';'); ch(';'); ch(')');
                    ch('{');
                    put_label(f, "Lb", f->ctrl[f->depth - 1].label);
                    f->ctrl[f->depth - 1].back_edge = 1;
                    emit_loop_check(f);
                } else {
                    ch('{');
                }
            }
            break;
        }

        case 0x04: {    /* if */
            uint8_t bt = rd_u8(f);
            WType rt = (bt == 0x7f) ? T_I32 : (bt == 0x7e) ? T_I64 :
                       (bt == 0x7d) ? T_F32 : (bt == 0x7c) ? T_F64 : T_NONE;
            int cond = popreg(f, T_I32);

            if (!f->ok || !ctrl_push(f, 2, rt != T_NONE, rt))
                return;
            if (!f->quiet) {
                kw(WK_IF);
                ch('(');
                put_reg(f, cond);
                ch(')');
                ch('{');
            }
            break;
        }

        case 0x05: {    /* else */
            if (f->depth <= 0 || f->ctrl[f->depth - 1].kind != 2) {
                fail(f, "else without if");
                return;
            }
            /* the taken side's value is written before switching sides */
            if (f->ctrl[f->depth - 1].has_result &&
                f->sp > f->ctrl[f->depth - 1].sp_at_entry) {
                put_assign(f, f->ctrl[f->depth - 1].result_reg);
                put_reg(f, f->stk_reg[f->sp - 1]);
                put_semi(f);
            }
            f->sp = f->ctrl[f->depth - 1].sp_at_entry;
            if (!f->quiet) {
                ch('}');
                kw(WK_ELSE);
                ch('{');
            }
            break;
        }

        case 0x0b: {    /* end */
            int kind;

            if (f->depth <= 0) {
                fail(f, "end without a block");
                return;
            }
            kind = f->ctrl[f->depth - 1].kind;
            if (kind == 3) {
                /* The function body: its `{` was the function's own, so this
                   must not emit one. The result is what the body left on the
                   stack. */
                struct Ctrl *c = &f->ctrl[f->depth - 1];

                if (c->has_result && f->sp > c->sp_at_entry && !f->quiet) {
                    kw(WK_RETURN);
                    put_reg(f, f->stk_reg[f->sp - 1]);
                    ch(';');
                } else if (!f->quiet) {
                    kw(WK_RETURN);
                    inum(0);
                    ch(';');
                }
                f->sp = c->sp_at_entry;
                f->depth--;
                return;
            }
            close_ctrl(f, 1);
            if (!f->ok)
                return;
            break;
        }

        case 0x0c: {    /* br */
            emit_br(f, rd_u32(f));
            break;
        }
        case 0x0d: {    /* br_if */
            uint32_t d = rd_u32(f);
            int cond = popreg(f, T_I32);

            if (!f->ok)
                break;
            emit_br_if(f, d, cond);
            if (!f->ok)
                return;
            break;
        }

        case 0x1a: {    /* drop */
            /* The value's temporary is simply left unused: no C statement is
               needed, and the register allocator already reserved the slot. */
            (void)popreg(f, f->sp > 0 ? f->stk_type[f->sp - 1] : T_I32);
            if (!f->ok)
                return;
            break;
        }

        case 0x1b: {    /* select */
            int cond = popreg(f, T_I32);
            int b, a, r;
            WType t;

            if (!f->ok)
                return;
            if (f->sp < 2) {
                fail(f, "select underflow");
                return;
            }
            t = f->stk_type[f->sp - 1];
            b = popreg(f, t);
            if (!f->ok)
                return;
            a = popreg(f, t);
            if (!f->ok)
                return;
            r = pushreg(f, t);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            put_reg(f, cond);
            ch('?');
            put_reg(f, a);
            ch(':');
            put_reg(f, b);
            ch(';');
            break;
        }

        case 0x41: {    /* i32.const */
            int32_t v = (int32_t)rd_s64(f);
            int r = pushreg(f, T_I32);

            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            inum(v);
            ch(';');
            break;
        }
        case 0x42: {    /* i64.const */
            int64_t v = rd_s64(f);
            int r = pushreg(f, T_I64);

            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED);
            kw(WK_LONG);
            kw(WK_LONG);
            ch(')');
            dnum((double)(uint64_t)v);
            ch(';');
            break;
        }
        case 0x43: {    /* f32.const */
            double v = rd_f32(f);
            int r = pushreg(f, T_F32);

            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            dnum(v);
            ch(';');
            break;
        }
        case 0x44: {    /* f64.const */
            double v = rd_f64(f);
            int r = pushreg(f, T_F64);

            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            dnum(v);
            ch(';');
            break;
        }

        case 0x20: {    /* local.get */
            uint32_t idx = rd_u32(f);
            int r;

            if ((int)idx >= f->nlocals) {
                fail(f, "local.get out of range");
                return;
            }
            r = pushreg(f, f->local_type[idx]);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            id(f->local_name[idx]);
            ch(';');
            break;
        }
        case 0x21: {    /* local.set */
            uint32_t idx = rd_u32(f);
            int r;

            if ((int)idx >= f->nlocals) {
                fail(f, "local.set out of range");
                return;
            }
            r = popreg(f, f->local_type[idx]);
            if (!f->ok || f->quiet)
                break;
            id(f->local_name[idx]);
            ch('=');
            put_reg(f, r);
            ch(';');
            break;
        }
        case 0x22: {    /* local.tee */
            uint32_t idx = rd_u32(f);

            if ((int)idx >= f->nlocals || f->sp <= 0) {
                fail(f, "local.tee out of range");
                return;
            }
            if (f->stk_type[f->sp - 1] != f->local_type[idx]) {
                fail(f, "local.tee type mismatch");
                return;
            }
            if (f->quiet)
                break;
            id(f->local_name[idx]);
            ch('=');
            put_reg(f, f->stk_reg[f->sp - 1]);
            ch(';');
            break;
        }

        case 0x0f: {    /* return */
            if (f->nresults > 0 && f->sp > 0) {
                if (!f->quiet) {
                    kw(WK_RETURN);
                    put_reg(f, f->stk_reg[f->sp - 1]);
                    ch(';');
                }
                f->sp--;
            } else if (!f->quiet) {
                kw(WK_RETURN);
                inum(0);
                ch(';');
            }
            break;
        }

        case 0x10:      /* call */
            emit_call_direct(f, rd_u32(f), 0);
            if (!f->ok)
                return;
            break;

        case 0x12:      /* return_call */
            emit_call_direct(f, rd_u32(f), 1);
            if (!f->ok)
                return;
            break;

        case 0x23: {    /* global.get */
            uint32_t gi = rd_u32(f);
            int t, r;

            if (!f->mod || (int)gi >= f->mod->nglobals ||
                f->mod->global_type[gi] == 0) {
                fail(f, "global.get on an unsupported global");
                return;
            }
            t = f->mod->global_type[gi];
            r = pushreg(f, (WType)t);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            if (t == T_F32 || t == T_F64) {
                /* A float's bits have to come out of the slot unchanged, which
                   a cast would not do. */
                id(t == T_F32 ? "whale_wasm_gget32" : "whale_wasm_gget64");
                ch('(');
                id("glob");
                ch(',');
                inum((int)gi);
                ch(')');
                ch(';');
                break;
            }
            ch('(');
            put_type_kw(t);
            ch(')');
            id("glob");
            ch('[');
            inum((int)gi);
            ch(']');
            ch(';');
            break;
        }

        case 0x24: {    /* global.set */
            uint32_t gi = rd_u32(f);
            int t, r;

            if (!f->mod || (int)gi >= f->mod->nglobals ||
                f->mod->global_type[gi] == 0) {
                fail(f, "global.set on an unsupported global");
                return;
            }
            t = f->mod->global_type[gi];
            r = popreg(f, (WType)t);
            if (!f->ok || f->quiet)
                break;
            if (t == T_F32 || t == T_F64) {
                id(t == T_F32 ? "whale_wasm_gset32" : "whale_wasm_gset64");
                ch('(');
                id("glob");
                ch(',');
                inum((int)gi);
                ch(',');
                put_reg(f, r);
                ch(')');
                ch(';');
                break;
            }
            id("glob");
            ch('[');
            inum((int)gi);
            ch(']');
            ch('=');
            put_reg(f, r);
            ch(';');
            break;
        }

        case 0x3f: {    /* memory.size */
            int r;

            (void)rd_u8(f);             /* the reserved zero byte */
            r = pushreg(f, T_I32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            /* `memsize >> 16` would be the natural spelling, but the token
               interface has no `>>` the parser accepts; a division is the same
               value for the unsigned size and goes through the plain '/' token. */
            id("memsize");
            ch('/');
            inum(65536);
            ch(';');
            break;
        }

        case 0x45: {    /* i32.eqz */
            int a = popreg(f, T_I32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('!');
            put_reg(f, a);
            ch(';');
            break;
        }

        /* memory.grow moves the linear memory block, so the host has to be
           told and the translated code's cached `mem` would go stale. The
           binding handles grow through its own exported wrapper, and a
           translated function that grows keeps its wasm3 body. */
        case 0x40:
            (void)rd_u8(f);
            fail(f, "memory.grow not translated");
            return;

        /* Numeric conversions, in the spec's order:
             0xa7            i32.wrap_i64
             0xa8..0xab      i32.trunc_f{32,64}_{s,u}      (trapping)
             0xac..0xad      i64.extend_i32_{s,u}
             0xae..0xb1      i64.trunc_f{32,64}_{s,u}      (trapping)
             0xb2..0xb5      f32.convert_i{32,64}_{s,u}
             0xb6            f32.demote_f64
             0xb7..0xba      f64.convert_i{32,64}_{s,u}
             0xbb            f64.promote_f32
             0xbc..0xbf      reinterpret
           The saturating truncations live behind the 0xfc prefix.
           Integer -> float is a plain cast: both round to nearest-even, so C
           agrees with the spec except for u64 -> float, which needs a helper.
           Float -> integer traps on NaN and overflow, which C leaves undefined,
           so those are helpers too. */
        case 0xa7: {    /* i32.wrap_i64 */
            int a = popreg(f, T_I64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED); kw(WK_INT);
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xa8: case 0xa9: case 0xaa: case 0xab:
        case 0xae: case 0xaf: case 0xb0: case 0xb1: {
            int to64 = (opcode >= 0xae);
            int from_f32 = (opcode == 0xa8 || opcode == 0xa9 ||
                            opcode == 0xae || opcode == 0xaf);
            int is_signed = (opcode == 0xa8 || opcode == 0xaa ||
                             opcode == 0xae || opcode == 0xb0);
            int a = popreg(f, from_f32 ? T_F32 : T_F64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, to64 ? T_I64 : T_I32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            id(to64 ? (is_signed ? "whale_trunc64_s" : "whale_trunc64_u")
                    : (is_signed ? "whale_trunc32_s" : "whale_trunc32_u"));
            ch('(');
            put_reg(f, a);
            ch(')');
            ch(';');
            break;
        }
        case 0xac: {    /* i64.extend_i32_s */
            int a = popreg(f, T_I32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_LONG); kw(WK_LONG);
            ch(')');
            ch('(');
            kw(WK_INT);
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xad: {    /* i64.extend_i32_u */
            int a = popreg(f, T_I32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG);
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xb2: case 0xb3: case 0xb4: case 0xb5:
        case 0xb7: case 0xb8: case 0xb9: case 0xba: {
            int to_f64 = (opcode >= 0xb7);
            int from64 = (opcode == 0xb4 || opcode == 0xb5 ||
                          opcode == 0xb9 || opcode == 0xba);
            int is_signed = (opcode == 0xb2 || opcode == 0xb4 ||
                             opcode == 0xb7 || opcode == 0xb9);
            int a = popreg(f, from64 ? T_I64 : T_I32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, to_f64 ? T_F64 : T_F32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            /* u64 -> float: no C cast rounds this correctly, and the helper is
               the same one the f32 and f64 directions need */
            if (!is_signed && from64) {
                id(to_f64 ? "whale_u64_to_f64" : "whale_u64_to_f32");
                ch('(');
                put_reg(f, a);
                ch(')');
                ch(';');
                break;
            }
            ch('(');
            if (to_f64)
                kw(WK_DOUBLE);
            else
                kw(WK_FLOAT);
            ch(')');
            if (is_signed) {
                ch('(');
                if (from64) {
                    kw(WK_LONG); kw(WK_LONG);
                } else {
                    kw(WK_INT);
                }
                ch(')');
            }
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xb6: {    /* f32.demote_f64 */
            int a = popreg(f, T_F64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_F32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_FLOAT);
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xbb: {    /* f64.promote_f32 */
            int a = popreg(f, T_F32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_F64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_DOUBLE);
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xbc: {    /* i32.reinterpret_f32 */
            int a = popreg(f, T_F32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            id("whale_f32_bits");
            ch('(');
            put_reg(f, a);
            ch(')');
            ch(';');
            break;
        }
        case 0xbd: {    /* i64.reinterpret_f64 */
            int a = popreg(f, T_F64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            id("whale_f64_bits");
            ch('(');
            put_reg(f, a);
            ch(')');
            ch(';');
            break;
        }
        case 0xbe: {    /* f32.reinterpret_i32 */
            int a = popreg(f, T_I32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_F32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            id("whale_bits_f32");
            ch('(');
            put_reg(f, a);
            ch(')');
            ch(';');
            break;
        }
        case 0xbf: {    /* f64.reinterpret_i64 */
            int a = popreg(f, T_I64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_F64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            id("whale_bits_f64");
            ch('(');
            put_reg(f, a);
            ch(')');
            ch(';');
            break;
        }
        case 0xc0: {    /* i32.extend8_s */
            int a = popreg(f, T_I32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED); kw(WK_INT);
            ch(')');
            ch('(');
            kw(WK_INT);
            ch(')');
            ch('(');
            id("signed char");
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xc1: {    /* i32.extend16_s */
            int a = popreg(f, T_I32);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I32);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED); kw(WK_INT);
            ch(')');
            ch('(');
            kw(WK_INT);
            ch(')');
            ch('(');
            id("short");
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xc2: {    /* i64.extend8_s */
            int a = popreg(f, T_I64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG);
            ch(')');
            ch('(');
            kw(WK_LONG); kw(WK_LONG);
            ch(')');
            ch('(');
            id("signed char");
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xc3: {    /* i64.extend16_s */
            int a = popreg(f, T_I64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG);
            ch(')');
            ch('(');
            kw(WK_LONG); kw(WK_LONG);
            ch(')');
            ch('(');
            id("short");
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }
        case 0xc4: {    /* i64.extend32_s */
            int a = popreg(f, T_I64);
            int r;

            if (!f->ok)
                return;
            r = pushreg(f, T_I64);
            if (!f->ok || f->quiet)
                break;
            put_assign(f, r);
            ch('(');
            kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG);
            ch(')');
            ch('(');
            kw(WK_LONG); kw(WK_LONG);
            ch(')');
            ch('(');
            kw(WK_INT);
            ch(')');
            put_reg(f, a);
            ch(';');
            break;
        }

        default:
            if (opcode >= 0x28 && opcode <= 0x35) {         /* loads */
                uint32_t align = rd_u32(f);
                uint32_t off = rd_u32(f);
                const char *ct = load_type(opcode);
                WType rt = load_result(opcode);
                int base, addr, r;

                (void)align;
                if (!ct) {
                    fail(f, "unsupported load");
                    return;
                }
                base = popreg(f, T_I32);
                if (!f->ok)
                    return;
                addr = newreg(f, T_I32);
                r = pushreg(f, rt);
                if (!f->ok || f->quiet)
                    break;
                emit_addr(f, addr, base, off);
                emit_bounds(f, addr, width_of(opcode));
                put_assign(f, r);
                ch('*');
                ch('(');
                put_load_type(f, ct);
                ch('*');
                ch(')');
                ch('(');
                id("mem");
                op(OP_ADD);
                put_reg(f, addr);
                ch(')');
                ch(';');
                break;
            }

            if (opcode >= 0x36 && opcode <= 0x3e) {         /* stores */
                uint32_t align = rd_u32(f);
                uint32_t off = rd_u32(f);
                WType vt = store_value(opcode);
                const char *ct = store_type(opcode);
                int v, base, addr;

                (void)align;
                if (!ct || vt == T_NONE) {
                    fail(f, "unsupported store");
                    return;
                }
                v = popreg(f, vt);
                if (!f->ok)
                    return;
                base = popreg(f, T_I32);
                if (!f->ok)
                    return;
                addr = newreg(f, T_I32);
                if (!f->ok || f->quiet)
                    break;
                emit_addr(f, addr, base, off);
                emit_bounds(f, addr, width_of(opcode));
                ch('*');
                ch('(');
                put_load_type(f, ct);
                ch('*');
                ch(')');
                ch('(');
                id("mem");
                op(OP_ADD);
                put_reg(f, addr);
                ch(')');
                ch('=');
                put_reg(f, v);
                ch(';');
                break;
            }

            {
                const OpDef *d = find_op(opcode);

                if (!d) {
                    char msg[64];

                    snprintf(msg, sizeof(msg), "unsupported opcode 0x%02x", opcode);
                    fail(f, msg);
                    return;
                }
                {
                    int b = popreg(f, d->type);
                    int a;
                    int r;

                    if (!f->ok)
                        return;
                    a = popreg(f, d->type);
                    if (!f->ok)
                        return;
                    r = pushreg(f, d->cond ? T_I32 : d->type);
                    if (!f->ok || f->quiet)
                        break;
                    put_assign(f, r);
                    if (d->helper)
                        emit_call2(f, d->helper, a, b);
                    else if (d->cond)
                        emit_cmp(f, d->c_op, a, b);
                    else
                        emit_bin(f, d->c_op, a, b);
                    ch(';');
                }
            }
            break;
        }
    }
}

/* Every helper the generated code may call, with the exact type it returns.
   TinyCC needs the prototype before the call: a symbol registered with
   JS_WhaleAddHostSymbol is not a declaration, and without these it assumes
   `int fn()` and rejects `double` results. */
typedef struct Helper {
    const char *sig;        /* return type, keyword spelling */
    const char *name;
    const char *params;     /* parameter type spellings, or "" */
} Helper;

static const Helper helpers[] = {
    { "int",                 "whale_wasm_trap",  "" },
    { "int",                 "whale_wasm_check_interrupt", "" },
    { "void",                "whale_wasm_note_interrupt", "" },
    { "int",                 "whale_div_trapped", "" },
    { "int",                 "whale_wasm_trapped", "" },
    { "int",                 "whale_sdiv32",     "u,u" },
    { "int",                 "whale_srem32",     "u,u" },
    { "unsigned long long",  "whale_sdiv64",     "U,U" },
    { "unsigned long long",  "whale_srem64",     "U,U" },
    { "unsigned int",        "whale_shl32",      "u,u" },
    { "unsigned int",        "whale_shr32",      "u,u" },
    { "unsigned int",        "whale_shrs32",     "u,u" },
    { "unsigned long long",  "whale_shl64",      "U,U" },
    { "unsigned long long",  "whale_shr64",      "U,U" },
    { "unsigned long long",  "whale_shrs64",     "U,U" },
    { "unsigned int",        "whale_rotl32",     "u,u" },
    { "unsigned int",        "whale_rotr32",     "u,u" },
    { "unsigned long long",  "whale_rotl64",     "U,U" },
    { "unsigned long long",  "whale_rotr64",     "U,U" },
    { "int",                 "whale_lts32",      "u,u" },
    { "unsigned int",        "whale_lt32",       "u,u" },
    { "int",                 "whale_gts32",      "u,u" },
    { "unsigned int",        "whale_gt32",       "u,u" },
    { "int",                 "whale_les32",      "u,u" },
    { "unsigned int",        "whale_le32",       "u,u" },
    { "int",                 "whale_ges32",      "u,u" },
    { "unsigned int",        "whale_ge32",       "u,u" },
    { "int",                 "whale_lts64",      "U,U" },
    { "unsigned int",        "whale_lt64",       "U,U" },
    { "int",                 "whale_gts64",      "U,U" },
    { "unsigned int",        "whale_gt64",       "U,U" },
    { "int",                 "whale_les64",      "U,U" },
    { "unsigned int",        "whale_le64",       "U,U" },
    { "int",                 "whale_ges64",      "U,U" },
    { "unsigned int",        "whale_ge64",       "U,U" },
    { "double",              "whale_min",        "d,d" },
    { "double",              "whale_max",        "d,d" },
    { "double",              "whale_copysign",   "d,d" },
    { "float",               "whale_minf",       "f,f" },
    { "float",               "whale_maxf",       "f,f" },
    { "float",               "whale_copysignf",  "f,f" },
    /* globals: the index argument is a plain int, so these carry their own
       spelling rather than the single-letter type code. "G" is the slot array
       parameter, and the letter after it is the value type. */
    { "float",               "whale_wasm_gget32", "Gi" },
    { "double",              "whale_wasm_gget64", "GI" },
    { "void",                "whale_wasm_gset32", "Gif" },
    { "void",                "whale_wasm_gset64", "Gid" },
    /* the entry wrappers' slot conversions */
    { "float",               "whale_wasm_f32_from_slot", "U" },
    { "unsigned long long",  "whale_wasm_f32_to_slot",   "f" },
    { "double",              "whale_wasm_f64_from_slot", "U" },
    { "unsigned long long",  "whale_wasm_f64_to_slot",   "d" },
    /* conversions */
    { "int",                 "whale_trunc32_s",     "d" },
    { "int",                 "whale_trunc32_u",     "d" },
    { "unsigned long long",  "whale_trunc64_s",     "d" },
    { "unsigned long long",  "whale_trunc64_u",     "d" },
    { "int",                 "whale_trunc_sat32_s", "d" },
    { "int",                 "whale_trunc_sat32_u", "d" },
    { "unsigned long long",  "whale_trunc_sat64_s", "d" },
    { "unsigned long long",  "whale_trunc_sat64_u", "d" },
    { "float",               "whale_u64_to_f32",    "U" },
    { "double",              "whale_u64_to_f64",    "U" },
    { "unsigned int",        "whale_f32_bits",      "f" },
    { "unsigned long long",  "whale_f64_bits",      "d" },
    { "float",               "whale_bits_f32",      "u" },
    { "double",              "whale_bits_f64",      "U" },
};

static void put_sig_type(const char *sig)
{
    /* one letter per type: u = unsigned int, U = unsigned long long,
       d = double, f = float, i = int, G = the global slot array */
    switch (sig[0]) {
    case 'u': kw(WK_UNSIGNED); kw(WK_INT); break;
    case 'U': kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG); break;
    case 'd': kw(WK_DOUBLE); break;
    case 'f': kw(WK_FLOAT); break;
    case 'G': kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG); ch('*'); break;
    default:  kw(WK_INT); break;
    }
}

/* Emit prototypes for the helpers so TCC knows their signatures. Idempotent in
   the sense that a repeated prototype is legal C; one state accumulates them
   harmlessly. */
static void emit_helper_decls(FnCtx *f)
{
    size_t i;

    if (f->quiet)
        return;
    for (i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++) {
        const Helper *h = &helpers[i];
        const char *p = h->params;

        put_sig_type(h->sig);
        id(h->name);
        ch('(');
        if (!p) {
            kw(WK_VOID);
        } else if (!*p) {
            kw(WK_VOID);
        } else {
            int first = 1;
            size_t k = 0;

            /* params is a run of single-letter types separated by commas */
            while (p[k]) {
                if (p[k] == ',') {
                    k++;
                    continue;
                }
                if (!first)
                    ch(',');
                put_sig_type(&p[k]);
                first = 0;
                k++;
            }
        }
        ch(')');
        ch(';');
    }
}

/* ---- module table, driven by the binding -----------------------------
 * The binding owns the module: it parses the type/import/function sections,
 * knows every signature and which globals are representable, and iterates
 * translation to a fixpoint. This is only the storage.
 */
void *JS_WhaleWasmModuleNew(int nfuncs)
{
    WasmCModule *m;

    if (nfuncs <= 0 || nfuncs > WASM_C_MAX_FUNCS)
        return NULL;
    m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->nfuncs = nfuncs;
    return m;
}

void JS_WhaleWasmModuleFree(void *mod)
{
    free(mod);
}

void JS_WhaleWasmModuleSetFunc(void *mod, int index, const int *params,
                               int nparams, const int *results, int nresults,
                               const char *sym)
{
    WasmCModule *m = mod;
    int i;

    if (!m || index < 0 || index >= m->nfuncs)
        return;
    if (nparams > 8 || nresults > 1) {
        /* Not representable by the translator's call ABI; leaving the
           signature empty makes a call to it fail, which is the right answer. */
        m->sig[index].nparams = -1;
        return;
    }
    m->sig[index].nparams = nparams;
    m->sig[index].nresults = nresults;
    for (i = 0; i < nparams; i++)
        m->sig[index].params[i] = params[i];
    for (i = 0; i < nresults; i++)
        m->sig[index].results[i] = results[i];
    if (sym)
        snprintf(m->sym[index], sizeof(m->sym[index]), "%s", sym);
}

void JS_WhaleWasmModuleSetGlobal(void *mod, int index, int type)
{
    WasmCModule *m = mod;

    if (!m || index < 0 || index >= WASM_C_MAX_GLOBALS)
        return;
    m->global_type[index] = type;
    if (index + 1 > m->nglobals)
        m->nglobals = index + 1;
}

void JS_WhaleWasmModuleMarkTranslated(void *mod, int index)
{
    WasmCModule *m = mod;

    if (m && index >= 0 && index < m->nfuncs)
        m->translated[index] = 1;
}

/* ---- public entry ---------------------------------------------------- */
typedef struct WasmCLazy {
    const uint8_t *body;
    size_t body_len;
    int nparams;
    WType params[32];
    int nresults;
    WType results[4];
    const char *name;
    WasmCModule *mod;
    int func_index;
    int ok;
    char err[192];
} WasmCLazy;

static WasmCLazy g_lazy;

/* Parse the local declarations, filling local_type/local_name. */
static int parse_locals(FnCtx *f)
{
    uint32_t nblocks = rd_u32(f);
    uint32_t i;

    for (i = 0; i < nblocks && f->ok; i++) {
        uint32_t count = rd_u32(f);
        uint8_t bt = rd_u8(f);
        WType t = (bt == 0x7f) ? T_I32 : (bt == 0x7e) ? T_I64 :
                  (bt == 0x7d) ? T_F32 : (bt == 0x7c) ? T_F64 : T_NONE;
        uint32_t k;

        if (t == T_NONE) {
            fail(f, "bad local type");
            return 0;
        }
        for (k = 0; k < count; k++) {
            if (f->nlocals >= MAX_LOCALS) {
                fail(f, "too many locals");
                return 0;
            }
            snprintf(f->local_name[f->nlocals], sizeof(f->local_name[0]), "l%d",
                     f->nlocals);
            f->local_type[f->nlocals] = t;
            f->nlocals++;
        }
    }
    return f->ok;
}

static void run_pass(FnCtx *f, int quiet)
{
    WasmCLazy *j = &g_lazy;
    int i;

    memset(f, 0, sizeof(*f));
    f->ok = 1;
    f->quiet = quiet;
    f->body = j->body;
    f->end = j->body + j->body_len;
    f->mod = j->mod;
    f->func_index = j->func_index;
    f->nparams = j->nparams;
    f->nresults = j->nresults;
    /* Labels are built from the function's own symbol, which the binding makes
       unique per (module, function). Two translated functions in the same
       TCCState must not share a label name. */
    snprintf(f->label_scope, sizeof(f->label_scope), "%s", j->name);
    for (i = 0; i < j->nparams; i++) {
        f->params[i] = j->params[i];
        f->local_type[i] = j->params[i];
        snprintf(f->local_name[i], sizeof(f->local_name[0]), "p%d", i);
    }
    f->nlocals = j->nparams;

    if (!parse_locals(f))
        return;

    /* The function body is the outermost block and always has a result slot
       when the signature does. */
    if (!ctrl_push(f, 3, f->nresults > 0, f->nresults > 0 ? j->results[0] : T_NONE))
        return;

    translate(f);
}

/* Emit the whole C function: header, declarations, body. */
static void emit_function(void *unused)
{
    WasmCLazy *j = &g_lazy;
    FnCtx pass1, pass2;
    int i;

    (void)unused;
    JS_WhaleTokReset();

    run_pass(&pass1, 1);
    if (!pass1.ok) {
        j->ok = 0;
        snprintf(j->err, sizeof(j->err), "%s", pass1.err);
        return;
    }

    /* signatures the body calls into: emitted for real, so the context must
       not be the quiet pass-1 one */
    {
        FnCtx decl_ctx;

        memset(&decl_ctx, 0, sizeof(decl_ctx));
        decl_ctx.ok = 1;
        decl_ctx.quiet = 0;
        emit_helper_decls(&decl_ctx);
    }

    /* prototypes for the sibling functions this body calls. Only the ones it
       actually calls: a module's functions all share one C unit, and declaring
       the whole module here would make a single untranslated sibling a compile
       error for everyone. */
    if (j->mod) {
        int k;

        for (k = 0; k < j->mod->nfuncs; k++) {
            WasmCFuncSig *s;
            int a;

            if (!pass1.callees[k])
                continue;
            s = &j->mod->sig[k];
            put_type_kw(s->nresults > 0 ? s->results[0] : T_I32);
            id(j->mod->sym[k]);
            ch('(');
            for (a = 0; a < s->nparams; a++) {
                if (a)
                    ch(',');
                put_type_kw(s->params[a]);
            }
            if (s->nparams)
                ch(',');
            kw(WK_UNSIGNED); kw(WK_CHAR); ch('*'); id("mem");
            ch(',');
            kw(WK_UNSIGNED); kw(WK_INT); id("memsize");
            ch(',');
            kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG); ch('*'); id("glob");
            ch(')');
            ch(';');
        }
    }

    /* signature */
    put_type_kw(j->nresults > 0 ? (int)j->results[0] : T_I32);
    id(j->name);
    ch('(');
    for (i = 0; i < j->nparams; i++) {
        if (i)
            ch(',');
        put_type_kw((int)j->params[i]);
        id(pass1.local_name[i]);
    }
    /* the linear memory and the globals come last and are not part of the wasm
       signature */
    if (j->nparams)
        ch(',');
    kw(WK_UNSIGNED);
    kw(WK_CHAR);
    ch('*');
    id("mem");
    ch(',');
    kw(WK_UNSIGNED);
    kw(WK_INT);
    id("memsize");
    ch(',');
    kw(WK_UNSIGNED);
    kw(WK_LONG);
    kw(WK_LONG);
    ch('*');
    id("glob");
    ch(')');
    ch('{');

    /* locals and temporaries */
    for (i = j->nparams; i < pass1.nlocals; i++) {
        put_type_kw((int)pass1.local_type[i]);
        id(pass1.local_name[i]);
        ch('=');
        inum(0);
        ch(';');
    }
    for (i = 0; i < pass1.nregs; i++) {
        char nm[16];

        snprintf(nm, sizeof(nm), "v%d", i);
        put_type_kw((int)pass1.reg_type[i]);
        id(nm);
        ch('=');
        inum(0);
        ch(';');
    }

    /* body: second pass writes it */
    run_pass(&pass2, 0);
    if (!pass2.ok) {
        /* pass 2 must agree with pass 1; if it does not, the stream already
           holds a partial function and the caller has to drop it */
        j->ok = 0;
        snprintf(j->err, sizeof(j->err), "pass2: %s", pass2.err);
        return;
    }
    /* translate() emits the closing `}` of the body block; close the function */
    ch('}');

    /* The uniform entry wrapper.
     *
     * A wasm signature is any mix of four types in any order, and C has no way
     * to call an unknown one. So each translated body also gets a wrapper that
     * speaks one fixed ABI - slots in, one slot out - and the binding calls
     * through that. It costs one jump per JS->wasm call, and it means the
     * dispatcher in whale_wasm.c does not have to enumerate signatures at all.
     * Floats travel as bits; a cast would convert them instead of moving them.
     */
    {
        char wname[64];
        int a;

        snprintf(wname, sizeof(wname), "%s__w", j->name);
        kw(WK_VOID);
        id(wname);
        ch('(');
        kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG); ch('*'); id("a");
        ch(',');
        kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG); ch('*'); id("r");
        ch(',');
        kw(WK_UNSIGNED); kw(WK_CHAR); ch('*'); id("mem");
        ch(',');
        kw(WK_UNSIGNED); kw(WK_INT); id("memsize");
        ch(',');
        kw(WK_UNSIGNED); kw(WK_LONG); kw(WK_LONG); ch('*'); id("glob");
        ch(')');
        ch('{');
        if (j->nresults > 0) {
            int rt = (int)j->results[0];

            id("r");
            ch('[');
            inum(0);
            ch(']');
            ch('=');
            if (rt == T_F32)
                id("whale_wasm_f32_to_slot"), ch('(');
            else if (rt == T_F64)
                id("whale_wasm_f64_to_slot"), ch('(');
            else if (rt == T_I32)
                ch('('), kw(WK_UNSIGNED), kw(WK_LONG), kw(WK_LONG), ch(')');
        }
        id(j->name);
        ch('(');
        for (a = 0; a < j->nparams; a++) {
            int pt = (int)j->params[a];

            if (a)
                ch(',');
            if (pt == T_F32)
                id("whale_wasm_f32_from_slot"), ch('('), id("a"), ch('['), inum(a), ch(']'), ch(')');
            else if (pt == T_F64)
                id("whale_wasm_f64_from_slot"), ch('('), id("a"), ch('['), inum(a), ch(']'), ch(')');
            else if (pt == T_I32)
                ch('('), kw(WK_UNSIGNED), kw(WK_INT), ch(')'), id("a"), ch('['), inum(a), ch(']');
            else
                id("a"), ch('['), inum(a), ch(']');
        }
        if (j->nparams)
            ch(',');
        id("mem");
        ch(',');
        id("memsize");
        ch(',');
        id("glob");
        ch(')');
        if (j->nresults > 0 && (j->results[0] == T_F32 || j->results[0] == T_F64))
            ch(')');
        ch(';');
        ch('}');
    }
    j->ok = 1;
}
/* Translate one function body and compile it into the engine's TinyCC state.
 *
 * This compiles only - the code is not runnable until the caller relocates the
 * shared state once for the whole batch (JS_WhaleRelocateExtern) and then looks
 * the symbol up with JS_WhaleFindSymbol. Relocating per function would move
 * every function compiled before it.
 *
 * `mod` carries the sibling signatures a `call` needs, and `translated[]` says
 * which siblings already went through the translator. A call to one that did
 * not makes this body fail too: mixing a native caller with an interpreted
 * callee would need a re-entry path this translator does not have. The caller
 * is expected to iterate until that fixpoint is reached.
 *
 * Returns 1 when the body was emitted and parsed. */
int JS_WhaleWasmTranslate(const char *name, const uint8_t *body, size_t body_len,
                          int nparams, const int *params,
                          int nresults, const int *results,
                          void *mod_ctx, int func_index)
{
    void *fn = NULL;
    int i;

    /* The engine resolves the translated code's external calls against its own
       host symbol table, so the helpers have to be registered there. Once per
       process is enough; registering twice is harmless. */
    if (!whale_helpers_registered) {
        static const struct { const char *name; void *addr; } reg[] = {
            { "whale_wasm_trap", (void *)whale_wasm_trap },
            { "whale_wasm_check_interrupt", (void *)whale_wasm_check_interrupt },
            { "whale_wasm_note_interrupt", (void *)whale_wasm_note_interrupt },
            { "whale_wasm_gget32", (void *)whale_wasm_gget32 },
            { "whale_wasm_gset32", (void *)whale_wasm_gset32 },
            { "whale_wasm_gget64", (void *)whale_wasm_gget64 },
            { "whale_wasm_gset64", (void *)whale_wasm_gset64 },
            { "whale_wasm_f32_from_slot", (void *)whale_wasm_f32_from_slot },
            { "whale_wasm_f32_to_slot", (void *)whale_wasm_f32_to_slot },
            { "whale_wasm_f64_from_slot", (void *)whale_wasm_f64_from_slot },
            { "whale_wasm_f64_to_slot", (void *)whale_wasm_f64_to_slot },
            { "whale_trunc32_s", (void *)whale_trunc32_s },
            { "whale_trunc32_u", (void *)whale_trunc32_u },
            { "whale_trunc64_s", (void *)whale_trunc64_s },
            { "whale_trunc64_u", (void *)whale_trunc64_u },
            { "whale_trunc_sat32_s", (void *)whale_trunc_sat32_s },
            { "whale_trunc_sat32_u", (void *)whale_trunc_sat32_u },
            { "whale_trunc_sat64_s", (void *)whale_trunc_sat64_s },
            { "whale_trunc_sat64_u", (void *)whale_trunc_sat64_u },
            { "whale_u64_to_f32", (void *)whale_u64_to_f32 },
            { "whale_u64_to_f64", (void *)whale_u64_to_f64 },
            { "whale_f32_bits", (void *)whale_f32_bits },
            { "whale_f64_bits", (void *)whale_f64_bits },
            { "whale_bits_f32", (void *)whale_bits_f32 },
            { "whale_bits_f64", (void *)whale_bits_f64 },
            { "whale_div_trapped", (void *)whale_div_trapped },
            { "whale_wasm_trapped", (void *)whale_wasm_trapped },
            { "whale_sdiv32", (void *)whale_sdiv32 },
            { "whale_srem32", (void *)whale_srem32 },
            { "whale_sdiv64", (void *)whale_sdiv64 },
            { "whale_srem64", (void *)whale_srem64 },
            { "whale_shl32", (void *)whale_shl32 },
            { "whale_shr32", (void *)whale_shr32 },
            { "whale_shrs32", (void *)whale_shrs32 },
            { "whale_shl64", (void *)whale_shl64 },
            { "whale_shr64", (void *)whale_shr64 },
            { "whale_shrs64", (void *)whale_shrs64 },
            { "whale_rotl32", (void *)whale_rotl32 },
            { "whale_rotr32", (void *)whale_rotr32 },
            { "whale_rotl64", (void *)whale_rotl64 },
            { "whale_rotr64", (void *)whale_rotr64 },
            { "whale_lts32", (void *)whale_lts32 },
            { "whale_lt32", (void *)whale_lt32 },
            { "whale_gts32", (void *)whale_gts32 },
            { "whale_gt32", (void *)whale_gt32 },
            { "whale_les32", (void *)whale_les32 },
            { "whale_le32", (void *)whale_le32 },
            { "whale_ges32", (void *)whale_ges32 },
            { "whale_ge32", (void *)whale_ge32 },
            { "whale_lts64", (void *)whale_lts64 },
            { "whale_lt64", (void *)whale_lt64 },
            { "whale_gts64", (void *)whale_gts64 },
            { "whale_gt64", (void *)whale_gt64 },
            { "whale_les64", (void *)whale_les64 },
            { "whale_le64", (void *)whale_le64 },
            { "whale_ges64", (void *)whale_ges64 },
            { "whale_ge64", (void *)whale_ge64 },
            { "whale_min", (void *)whale_min },
            { "whale_max", (void *)whale_max },
            { "whale_copysign", (void *)whale_copysign },
            { "whale_minf", (void *)whale_minf },
            { "whale_maxf", (void *)whale_maxf },
            { "whale_copysignf", (void *)whale_copysignf },
        };
        size_t k;

        for (k = 0; k < sizeof(reg) / sizeof(reg[0]); k++)
            JS_WhaleAddHostSymbol(reg[k].name, reg[k].addr);
        whale_helpers_registered = 1;
    }

    memset(&g_lazy, 0, sizeof(g_lazy));
    g_lazy.name = name;
    g_lazy.body = body;
    g_lazy.body_len = body_len;
    g_lazy.nparams = nparams;
    g_lazy.nresults = nresults;
    g_lazy.mod = (WasmCModule *)mod_ctx;
    g_lazy.func_index = func_index;
    if (func_index >= 0 && func_index < WASM_C_MAX_FUNCS)
        g_call_blocked[func_index] = 0;
    for (i = 0; i < nparams && i < 32; i++)
        g_lazy.params[i] = (WType)params[i];
    for (i = 0; i < nresults && i < 4; i++)
        g_lazy.results[i] = (WType)results[i];

    if (!JS_WhaleCompileExtern(emit_function, NULL, name, &fn)) {
        if (whale_wasm_c_dbg()) {
            size_t k;

            fprintf(stderr, "[wasm-c] %s failed: %s\n  body:", name,
                    g_lazy.ok ? "compile" : g_lazy.err);
            for (k = 0; k < body_len && k < 48; k++)
                fprintf(stderr, " %02x", body[k]);
            fprintf(stderr, "\n");
        }
        return 0;
    }
    if (!g_lazy.ok)
        return 0;
    if (whale_wasm_c_dbg())
        fprintf(stderr, "[wasm-c] %s translated\n", name);
    return 1;
}

