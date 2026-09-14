/*
 * WhaleJS: JIT code generation (TinyCC backend)
 *
 * This is the whole code generator, kept in its own translation unit so that
 * editing it rebuilds ~600 lines instead of all of quickjs.c. It knows nothing
 * about quickjs internals: the engine hands it a JSWhaleFuncInfo, and the few
 * internal services it needs come through the small JS_Whale* wrappers that
 * quickjs.c exports.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "quickjs.h"
#include "whale_jit.h"

#include "libtcc.h"

/* implemented inside the TinyCC translation unit (src/tcc/whale_tcc_iface.c) */
int JS_WhaleTccProbe(void);
void JS_WhaleTokPush(int t, double val);
/* the generator folds expressions in a workspace it owns and then hands the
   linearised token stream to the parser through Commit() */
void JS_WhaleTokSink(int *tok, double *num, int cap);
int  JS_WhaleTokSinkLen(void);
void JS_WhaleTokSinkTruncate(int n);
void JS_WhaleTokCommit(int *tok, double *num, int n);
extern int whale_tok_active;   /* owned by the TinyCC-side file */
extern int whale_gen_ok;       /* set by whale_gen_cb, read by the interface */
extern int whale_gen_pure;     /* 0 once a global read is emitted (no memoising) */
extern int whale_gen_objs;     /* object slots the emitted function uses (0 = none) */
int JS_WhaleCompileTokens(struct TCCState *s, void (*emit)(void *), void *emit_ctx);

/* WhaleJS services implemented in quickjs.c. The generated code only *names*
   the runtime helpers (they are emitted as tokens), but tcc_add_symbol() below
   takes their real addresses, so these need declarations. */
int JS_WhaleToInt32(double d);
int JS_WhaleGetGlobalNum(JSContext *ctx, int atom, double *out);

/* Opcode ids and sizes, taken from the same quickjs-opcode.h the interpreter
   uses. The enum mirrors OPCodeEnum in quickjs.c: the DEF entries first, then
   the short (def) entries that alias the temporary opcode range. */
typedef enum OPCodeEnum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_ ## id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_COUNT,
    OP_TEMP_START = OP_nop + 1,
    OP___dummy = OP_TEMP_START - 1,
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f)
#define def(id, size, n_pop, n_push, f) OP_ ## id,
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_TEMP_END,
} OPCodeEnum;

/* Sizes come from the engine (short_opcode_info) so they can never drift from
   the interpreter's own table. */
static int whale_op_size_of(int op)
{
    return JS_WhaleOpSize(op);
}

static uint32_t whale_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t whale_get_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
/* =========================================================================
 * WhaleJS: JIT code generation (TinyCC backend)
 *
 * Shape compiled: functions that are numeric end to end, plus self-recursive
 * ones. An abstract interpreter walks the bytecode with the type lattice
 * {UNKNOWN, NUM, BOOL, SELF}; when every value reaching an arithmetic op, a
 * local read, a call argument or the return is NUM/BOOL, the whole function runs
 * in the C double domain (locals and stack slots become double C locals), so the
 * inner loop is a few machine instructions instead of a JSValue dance.
 *
 * Why double for everything numeric: a JS Number IS a double, so arithmetic in
 * the double domain is already spec behaviour, int32 overflow included. No range
 * checks and no deopt are needed for overflow - only for non-numbers.
 *
 * Recursion: `call` is only accepted when the callee is *this* function
 * (get_var_ref checked against the running function at run time). That keeps the
 * compiled function side-effect free, which is what makes deoptimization safe to
 * re-run: a mismatched type sets *deopt, JS_CallInternal drops the compiled entry
 * and simply interprets the same frame again.
 *
 * Optimisation notes (see also temp/tcc_to_optimize.md): TCC has no register
 * allocator worth the name and spills at every statement boundary, so the emitter
 * works hard to keep the variable count and the stack-slot round trips low:
 *   - <load> <load> <binop> [put_loc] folds into one assignment
 *   - <load> put_loc folds into a direct copy
 *   - arguments are unboxed once into A0..An
 * That is also why constants fold for free: they never become variables.
 * ========================================================================= */

#include "libtcc.h"

/* implemented inside the TinyCC translation unit (src/tcc/whale_tcc_iface.c) */
int JS_WhaleTccProbe(void);
void JS_WhaleTokPush(int t, double val);
extern int whale_tok_active;   /* owned by the TinyCC-side file */
int JS_WhaleCompileTokens(struct TCCState *s, void (*emit)(void *), void *emit_ctx);

#ifndef WHALE_JIT_TCC_DIR
#define WHALE_JIT_TCC_DIR "thirdparty/tcc-runtime"
#endif
#ifndef WHALE_JIT_SRC_DIR
#define WHALE_JIT_SRC_DIR "src"
#endif

#define WHALE_JIT_MAX_SLOTS 64

/* type lattice for the specialization pass */
enum {
    WHALE_T_UNKNOWN = 0,
    WHALE_T_NUM = 1,  /* Number: held as double while specialized */
    WHALE_T_BOOL = 2, /* true / false, usable as 1 / 0 */
    WHALE_T_SELF = 3, /* this function, usable only as a call target */
    WHALE_T_OBJ = 4,  /* a JS object (typed array). Only the typed-array helpers
                         may touch it; anything else deopts. */
    WHALE_T_I32 = 5,  /* a number already known to be an in-range int32, so ToInt32
                         on it is just a cast. Bitwise results always are. */
    WHALE_T_ARR = 6,  /* a packed array. Its elements are JSValue, so only the
                         structural parts go through the engine. */
    WHALE_T_ICONST = 7, /* a literal int32. ToInt32 of it is a cast to a known
                           constant, so the whole range test is unnecessary -
                           bitwise code is full of these (255, 3, 1, 0xffff). */
    WHALE_T_FN = 9,   /* a function this compilation can see through: the local
                         it sits in and the pool entry it came from are both
                         compile-time facts, so a call to it can be expanded. */
    WHALE_T_VAL = 8,  /* an arbitrary JSValue parked in an object slot: a global
                         function (a call target) or a property that is not a
                         number. Only a call may consume it - arithmetic on it
                         deopts, and that is what keeps it safe. */
    WHALE_T_STRUCT = 10, /* an object slot whose observed shape set is settled, held
                            as a C struct instead of a JSValue: no shape check, no
                            tag branch, no heap chase, one load per field. A field
                            that is not in every observed shape keeps its slot and
                            gets a presence flag - reserving eight bytes is cheap
                            next to the access it replaces. */
};

/* The object slot a type carries, packed into the same byte as the type.

   cur[] is uint8_t and is snapshotted at every jump target, so an object's slot
   number has to travel with its type rather than in a parallel array that would
   need its own snapshot handling. The low nibble holds the type, the high nibble
   the slot + 1 - which caps WHALE_OBJ_SLOTS at 15 and costs nothing, because an
   object's byte never equals WHALE_T_NUM/BOOL/SELF, so every existing type check
   rejects it automatically. */
#define WHALE_OBJ_T(slot)  ((uint8_t)(WHALE_T_OBJ | (((slot) + 1) << 4)))
#define WHALE_IS_OBJ(t)    (((t) & 0x0f) == WHALE_T_OBJ)
#define WHALE_OBJ_SLOT(t)  ((int)((t) >> 4) - 1)
/* a packed array carries its slot the same way an object does */
#define WHALE_ARR_T(slot)  ((uint8_t)(WHALE_T_ARR | (((slot) + 1) << 4)))
#define WHALE_IS_ARR(t)    (((t) & 0x0f) == WHALE_T_ARR)
/* an arbitrary JSValue parked in a slot, named the same way as an object's */
#define WHALE_VAL_T(slot)  ((uint8_t)(WHALE_T_VAL | (((slot) + 1) << 4)))
#define WHALE_IS_VAL(t)    (((t) & 0x0f) == WHALE_T_VAL)
#define WHALE_VAL_SLOT(t)  ((int)((t) >> 4) - 1)
/* an unboxed struct: the same slot numbering, a different thing living in it */
#define WHALE_STRUCT_T(slot) ((uint8_t)(WHALE_T_STRUCT | (((slot) + 1) << 4)))
#define WHALE_IS_STRUCT(t)   (((t) & 0x0f) == WHALE_T_STRUCT)
#define WHALE_STRUCT_SLOT(t) ((int)((t) >> 4) - 1)

/* Anything that carries an object slot - a plain object, a packed array, or an
   arbitrary value - encodes the slot the same way, so one accessor serves all
   three. WHALE_T_VAL only exists because a value from a call is not a literal
   whose shape we can see; the slot mechanics are identical.

   WHALE_T_STRUCT is deliberately NOT in here. It carries a slot number like the
   others, but what sits in that slot is a C struct and not a JSValue, so every
   existing site that would treat it as a parked value has to refuse it instead.
   Leaving it out of this test is what makes that happen without touching them. */
#define WHALE_IS_SLOT(t)   (WHALE_IS_OBJ(t) || WHALE_IS_ARR(t) || WHALE_IS_VAL(t))
#define WHALE_SLOT(t)      ((int)((t) >> 4) - 1)

/* ------------------------------------------------------------------------
 * Token emitters.
 *
 * The generator pushes tokens straight into TinyCC's parser - there is no text
 * stage and therefore no lexer, no keyword table and no literal scanner. The
 * keywords are compile-time constants here (TOK_DOUBLE, TOK_RETURN, ...), so
 * nothing can be mistaken for an identifier or collide with a TinyCC keyword.
 * ---------------------------------------------------------------------- */
/* wk = keyword (JS_WK_*), wt = punctuation / single-char operator */
static void wk(int w)
{
    JS_WhaleTokKw(w);
}

/* WhaleJS: with WHALE_JIT_SRC=<file> the generated C source is mirrored into that
   file as it is emitted. The disassembled code section can only be counted, not
   read - it is dumped before relocation, so objdump cannot name a single callee.
   The token wrappers below, on the other hand, are handed the literal text, so
   mirroring them reconstructs the source and makes it possible to see which helpers
   sit inside a loop body. */
static FILE *whale_src_fp;
static int whale_src_tried;

static void wsrc(const char *s)
{
    if (!whale_src_tried) {
        const char *p = getenv("WHALE_JIT_SRC");

        whale_src_tried = 1;
        if (p && *p)
            whale_src_fp = fopen(p, "w");
    }
    if (whale_src_fp)
        fputs(s, whale_src_fp);
}

static void wt(int t)
{
    if (whale_src_fp || !whale_src_tried) {
        char b[3];

        b[0] = (char)t; b[1] = (t == ';' || t == '{' || t == '}') ? '\n' : ' '; b[2] = 0;
        wsrc(b);
    }
    JS_WhaleTokChar(t);          /* punctuation / single-character operators */
}

static void wt_id(const char *name)
{
    if (whale_src_fp || !whale_src_tried) {
        char b[256];

        snprintf(b, sizeof b, "%s ", name);
        wsrc(b);
    }
    JS_WhaleTokIdent2(name);
}

static void wt_slot(const char *prefix, int n)
{
    if (whale_src_fp || !whale_src_tried) {
        char b[64];

        snprintf(b, sizeof b, "%s%d ", prefix, n);
        wsrc(b);
    }
    JS_WhaleTokSlot(prefix, n);  /* L0 / v3 / A2 */
}

static void wt_int(int v);

/* A slot of the generated function's state block, as `W.v[i]`. The block is one
   zero-initialised object: declaring and initialising each slot separately cost
   a statement per slot, and TinyCC spills at every statement, so a short
   function like fib spent almost all of its time in the prologue. */
static void wt_arr(const char *field, int n)
{
    wt_id("_ws"); wt('.'); wt_id(field); wt('['); wt_int(n); wt(']');
}

static void wt_op(int o)
{
    JS_WhaleTokOp(o);
}

static void wt_int(int v)
{
    if (whale_src_fp || !whale_src_tried) {
        char b[32];

        snprintf(b, sizeof b, "%d ", v);
        wsrc(b);
    }
    JS_WhaleTokInt(v);
}

static void wt_num(double v)
{
    if (whale_src_fp || !whale_src_tried) {
        char b[64];

        snprintf(b, sizeof b, "(double)%.17g ", v);
        wsrc(b);
    }
    JS_WhaleTokDouble(v);
}

/* ---- helpers the generated code calls back into ---- */

/* Is closure slot idx of the frame that is currently running equal to `self`?
   Only true for a direct self-recursive reference, which is the single call
   shape we compile. Borrowed reference: no dup/free needed. */

/* Self-recursive call: go back through the engine so the callee gets a proper
   frame. The callee is this same function object, so it re-enters this JIT
   entry rather than the interpreter. */

/* The process-wide TCCState plus the per-compile function names. The state is
   created on the first compile and lives until process exit; every later compile
   is another file inside it, so the generated names carry a counter to stay
   unique while earlier functions' code remains live. */
static TCCState *whale_jit_state;
static unsigned whale_jit_seq;
/* TCC side of the pinned-register map: one scope per JIT compilation. */
void JS_WhalePinEnter(void);
/* WhaleJS: dump the finished code section so it can be disassembled. Defined in
   tccgen.c, which is the only place text_section is in scope. */
void JS_WhaleDumpCode(FILE *f);

static char whale_core_name[40];
static char whale_entry_name[40];


static JSValue whale_jit_box_num(JSContext *ctx, double d)
{
    if (d >= -2147483648.0 && d <= 2147483647.0) {
        int i = (int)d;

        if ((double)i == d)
            return JS_MKVAL(JS_TAG_INT, i);
    }
    return JS_NewFloat64(ctx, d);
}

/* returns 1 and stores the value when v is a number */
static int whale_jit_unbox_num(JSContext *ctx, JSValue v, double *out)
{
    int t = JS_VALUE_GET_TAG(v);

    if (t == JS_TAG_INT) {
        *out = (double)JS_VALUE_GET_INT(v);
        return 1;
    }
    if (JS_TAG_IS_FLOAT64(t)) {
        *out = (double)JS_VALUE_GET_FLOAT64(v);
        return 1;
    }
    return 0;
}

/* Is this opcode a direct call? Used to tell a closure slot that is a
   self-recursive call target from one that is an enclosing function's
   captured variable. A switch rather than a range test, because opcodes
   generated by these macros are not guaranteed to be adjacent. */
static int whale_is_call_op(int op)
{
    switch (op) {
    case OP_call: case OP_call0: case OP_call1: case OP_call2: case OP_call3:
        return 1;
    default:
        return 0;
    }
}

/* Does the closure slot read at `pos` feed a call? The arguments in between
   mean the call is not the very next opcode - `fib(n-1)` reads the slot, then
   pushes the argument and subtracts, and only then calls - so a one-opcode
   lookahead misses it and the function stopped compiling entirely. Looking a
   few instructions ahead is enough, and being wrong here only means falling
   back to the self-recursion reading of the slot, which then bails out rather
   than computing something wrong. */
/* How a closure slot that feeds a call is read - see the note where it is
   defined (whale_tcc_iface.c). The engine compiles both readings when a
   function needs it, so this is set by the caller rather than sniffed here. */
extern int whale_assume_self;
/* 1 when this compilation put a "is this closure slot this function?" check in
   the entry, i.e. when the two readings above actually differ for this
   function. That is what tells the engine a second compilation is worth it. */
int whale_gen_self_ref = 0;

/* Does this instruction push a literal or a variable read? Used only to look a
   few instructions ahead for the `f(a, b)` shape. */
static int whale_is_push_op(int op)
{
    switch (op) {
    case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
    case OP_push_i8: case OP_push_i32:
    case OP_push_const: case OP_push_const8:
    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
    case OP_get_loc:
    case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
    case OP_get_arg:
        return 1;
    default:
        return 0;
    }
}

/* The local a store writes, or -1. */
static int whale_store_local(const uint8_t *pc, int len, int pos, int sz)
{
    int op = (int)pc[pos];

    (void)len; (void)sz;
    if (op >= OP_put_loc0 && op <= OP_put_loc3)
        return op - OP_put_loc0;
    if (op == OP_put_loc8)
        return (int)pc[pos + 1];
    if (op == OP_put_loc)
        return (int)whale_get_u16(pc + pos + 1);
    return -1;
}

/* Is the value this instruction pushes read as the *object* of a property
   access right after? `o.x` compiles to `get_arg0 o`, `get_field x`, so this is
   adjacency. Used to decide how an argument has to travel: a parameter that is
   the receiver of a property access cannot be a plain double. */
static int whale_next_is_field_read(const uint8_t *pc, int len, int pos, int sz)
{
    int k = pos + sz;

    if (k >= len)
        return 0;
    return pc[k] == OP_get_field || pc[k] == OP_get_field2;
}

/* Is the value this instruction leaves on the stack about to become the callee
   of a call? `f(x)` compiles to `get_var f`, `get_loc0 x`, `call1`, so the
   callee is not adjacent to the instruction that loads it. A short run of pure
   pushes followed by a call is the shape we look for.

   Being wrong here is safe by construction: the value is then marked
   WHALE_T_VAL, and the only thing that accepts a WHALE_T_VAL is a call - any
   arithmetic on it deopts, so the function is simply not compiled. */
static int whale_feeds_call_as_callee(const uint8_t *pc, int len, int pos, int sz)
{
    int k = pos + sz;
    int n = 0;

    while (k < len && n < 5) {
        int o = (int)pc[k];
        int s;

        if (whale_is_call_op(o))
            return 1;
        if (!whale_is_push_op(o))
            return 0;
        s = whale_op_size_of(o);
        if (s <= 0)
            return 0;
        k += s;
        n++;
    }
    return 0;
}

static int whale_var_ref_feeds_call(const uint8_t *pc, int len, int pos, int sz)
{
    int k = pos + sz;
    int n = 0;

    /* The window has to be wide enough to see past a real argument list, or a
       variable read as a call target gets mistaken for a number. 12 was not:
       `createElement(i % 5, { id: i, cls: "c" + (i % 3) })` needs more, and when
       this returned 0 the slot was typed WHALE_T_NUM - the value is a function, and
       the call then gave up for wanting a slot. Erring the other way is harmless:
       a number parked in a slot still works, it just costs a round trip. */
    while (k < len && n < 48) {
        int o = (int)pc[k];
        int s = whale_op_size_of(o);

        if (whale_is_call_op(o))
            return 1;
        if (s <= 0)
            return 0;
        k += s;
        n++;
    }
    return 0;
}

static int whale_type_is_num(int t)
{
    /* WHALE_T_I32 counts as a number: it only adds the knowledge that the value
       is already an in-range int32. */
    return t == WHALE_T_NUM || t == WHALE_T_BOOL || t == WHALE_T_I32 ||
           t == WHALE_T_ICONST;
}

/* numeric binary opcode -> C operator. Returns NULL when the opcode is not a
   plain numeric binary op (the emitter then refuses to JIT the function).
   `*is_cmp` tells the caller whether the result is a boolean. */
static int whale_binop_op(int op, int *is_cmp)
{
    *is_cmp = 0;
    switch (op) {
    case OP_add: return JS_OP_ADD;
    case OP_sub: return JS_OP_SUB;
    case OP_mul: return JS_OP_MUL;
    case OP_div: return JS_OP_DIV;
    case OP_lt:  *is_cmp = 1; return JS_OP_LT;
    case OP_lte: *is_cmp = 1; return JS_OP_LE;
    case OP_gt:  *is_cmp = 1; return JS_OP_GT;
    case OP_gte: *is_cmp = 1; return JS_OP_GE;
    case OP_eq: case OP_strict_eq:   *is_cmp = 1; return JS_OP_EQ;
    case OP_neq: case OP_strict_neq: *is_cmp = 1; return JS_OP_NE;
    default: return -1;
    }
}
/* A load-like opcode, resolved. The emitter needs two steps because the
   peepholes must look at two loads before deciding what to emit, and tokens
   cannot be pushed speculatively. */
enum {
    WLD_SLOT = 0,  /* local: L<idx> */
    WLD_ARG = 1,   /* argument: A<idx> */
    WLD_INT = 2,   /* integer constant in cval */
    WLD_REAL = 3,  /* floating constant in cval */
};

/* A token range in the folding workspace that produces one value. */
typedef struct { int off, len; } WhaleVExpr;

/* A local can be deferred like a stack slot (see the deferred-locals note next
   to the helpers). Declared here because whale_load_analyze() below has to know
   whether a local currently holds a pending expression rather than a value. */
typedef struct WhaleLocal {
    WhaleVExpr expr;
    uint8_t    live;    /* 1 = pending expression, 0 = read the variable */
    uint8_t    used;    /* the pending expression has been inlined once */
} WhaleLocal;

static WhaleLocal *whale_lmem;

/* Decide what this load means. Returns 0 if it is not a simple load. */
static int whale_load_analyze(const JSWhaleFuncInfo *info, const uint8_t *pc, int pos, const uint8_t *cur,
                              int var_count, const uint8_t *lo_c, const double *lo_cv,
                              int *len_out, int *type_out, int *kind_out, int *idx_out, double *cval_out)
{
    int op = pc[pos];

    *len_out = whale_op_size_of(op);
    *type_out = WHALE_T_NUM;
    *kind_out = WLD_INT;
    *idx_out = 0;
    *cval_out = 0;
    switch (op) {
    case OP_push_minus1:
        *cval_out = -1;
        return 1;
    case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
    case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
        *cval_out = (double)(op - OP_push_0);
        return 1;
    case OP_push_i8:
        *cval_out = (double)(int)(int8_t)pc[pos + 1];
        return 1;
    case OP_push_i16:
        *cval_out = (double)(int)(int16_t)whale_get_u16(pc + pos + 1);
        return 1;
    case OP_push_i32:
        *cval_out = (double)(int)(int32_t)whale_get_u32(pc + pos + 1);
        return 1;
    case OP_push_true:
        *type_out = WHALE_T_BOOL;
        *cval_out = 1;
        return 1;
    case OP_push_false:
        *type_out = WHALE_T_BOOL;
        *cval_out = 0;
        return 1;
    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
    case OP_get_loc8:
    case OP_get_loc:
        {
            int idx = (op == OP_get_loc8) ? (int)pc[pos + 1] :
                      (op == OP_get_loc) ? (int)whale_get_u16(pc + pos + 1) :
                      op - OP_get_loc0;
            int tt = (idx >= 0 && idx < var_count) ? cur[idx] : WHALE_T_UNKNOWN;

            if (tt == WHALE_T_UNKNOWN)
                return 0;
            /* A local whose value is still a pending expression must not be
               read as `L<idx>` here - that would be the stale value. Refuse, so
               the switch path goes through local_inline() instead. */
            if (idx >= 0 && idx < var_count && whale_lmem && whale_lmem[idx].live)
                return 0;
            *type_out = tt;
            /* constant propagation: a local written exactly once with a
               compile-time constant is that constant everywhere afterwards */
            if (idx >= 0 && idx < var_count && lo_c && lo_c[idx]) {
                *kind_out = WLD_REAL;
                *cval_out = lo_cv[idx];
            } else {
                *kind_out = WLD_SLOT;
                *idx_out = idx;
            }
            return 1;
        }
    case OP_push_const:
    case OP_push_const8:
        {
            int idx = (op == OP_push_const8) ? (int)pc[pos + 1] : (int)whale_get_u32(pc + pos + 1);
            JSValue c;
            int ct;

            if (!info || idx < 0 || idx >= info->cpool_count)
                return 0;
            c = info->cpool[idx];
            ct = JS_VALUE_GET_TAG(c);
            if (ct == JS_TAG_INT) {
                *cval_out = (double)JS_VALUE_GET_INT(c);
                return 1;
            }
            if (JS_TAG_IS_FLOAT64(ct)) {
                *cval_out = JS_VALUE_GET_FLOAT64(c);
                /* must say WLD_REAL: the default kind is WLD_INT, and
                   whale_load_emit() would then truncate the literal to int
                   (1.5 -> 1) and every float constant in the function would be
                   silently wrong. */
                *kind_out = WLD_REAL;
                return 1;
            }
            return 0; /* strings / bigints: not our shape */
        }
    case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
    case OP_get_arg:
        *kind_out = WLD_ARG;
        *idx_out = (op == OP_get_arg) ? (int)whale_get_u16(pc + pos + 1) : op - OP_get_arg0;
        return 1;
    default:
        return 0;
    }
}

/* push the resolved load as tokens */
static void whale_load_emit(int kind, int idx, double cval)
{
    switch (kind) {
    case WLD_SLOT: wt_slot("L", idx); break;
    case WLD_ARG:  wt_slot("A", idx); break;
    case WLD_INT:  wt_int((int)cval); break;
    default:       wt_num(cval); break;
    }
}

/* Opcode name for diagnostics, built from the same table the enum uses so it
   can never drift from the interpreter's numbering (the temp opcodes are named
   too, shifted by OP_TEMP_START as the enum does). */
static const char *whale_op_name(int op)
{
    static const char *def_names[] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) #id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    };
    static const char *tmp_names[] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f)
#define def(id, size, n_pop, n_push, f) #id,
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    };

    if (op >= 0 && op < OP_COUNT)
        return def_names[op];
    if (op >= OP_TEMP_START && op <= OP_TEMP_END)
        return tmp_names[op - OP_TEMP_START];
    return "?";
}

/* =====================================================================
 * Expression folding
 *
 * A stack slot is not written into a C variable until something forces it to
 * be. The generator instead keeps the token sequence that produces the value,
 * so a chain like
 *     s = s * c + k;
 * comes out as one C statement instead of four. That is worth a lot here:
 * TinyCC has no register allocator and spills at every statement boundary.
 *
 * Two buffers, both owned by this file:
 *   gw_*   append-only workspace - every encoder call lands here. A value
 *          expression is a [off,len) range into it, and because it only ever
 *          grows those ranges stay valid, so a value can be inlined again.
 *   go_*   the finished stream in final order. A statement is committed by
 *          copying its token range here; JS_WhaleTokCommit() then hands go_*
 *          to the parser.
 *
 * A statement's tokens are contiguous in gw_* (its inline copies are appended
 * as it is emitted), so committing is one copy of [mark, now).
 * ===================================================================== */
#define WHALE_TOK_MAX 32768

static int    gw_tok[WHALE_TOK_MAX];
static double gw_num[WHALE_TOK_MAX];
static int    go_tok[WHALE_TOK_MAX];
static double go_num[WHALE_TOK_MAX];
static int    go_len;

/* Where committed statements land. Normally the finished stream; while a loop
   body is being buffered it points at that loop's own buffer instead, so the
   whole body can be worked on before it is handed on. */
static int    *whale_out_tok = go_tok;
static double *whale_out_num = go_num;
static int    *whale_out_len = &go_len;

/* ---- loop buffering ------------------------------------------------------
 * A loop body is built in its own buffer and emitted in one go. Nothing is
 * optimised here yet - this is the plumbing that makes it possible, because
 * hoisting a loop invariant or unrolling both need to rearrange code that has
 * not been handed on already. Nesting uses a small stack.
 */
#define WHALE_LOOP_MAX 4
#define WHALE_LOOP_BUF 4096

typedef struct WhaleLoop {
    int     tok[WHALE_LOOP_BUF];
    double  num[WHALE_LOOP_BUF];
    int     len;
    int     head_pos;       /* bytecode offset of the loop head */
    int     back_pos;       /* ...of the backward jump that closes the body */
} WhaleLoop;

static WhaleLoop whale_loops[WHALE_LOOP_MAX];
static int whale_loop_depth;

static WhaleVExpr *whale_vexpr;   /* the pending expression of each stack slot */
static uint8_t    *whale_vmat;    /* 1 = already forced into a variable */

/* Which C variable a stack slot currently lives in.
   
   TinyCC has no register allocator, so a folded expression is spilled into a
   named double. That name must not be reused once anything has inlined it: a
   pending expression records the name as a token, so rewriting the variable
   afterwards would change the value the pending expression reads. Each
   materialisation therefore takes a fresh name, and `v<k>` is only ever the
   *first* one. References go through whale_vname[k]. */
#define WHALE_VSER_MAX 64
static int whale_vname[WHALE_JIT_MAX_SLOTS];  /* slot -> name number */
/* Diagnostics: how many times fold_inline_expr was handed a range that no longer
   points at an expression. See the note in its body. */
int whale_empty_ranges;
/* Which pool entry a local / stack slot holds a *function* for, or -1. Filled by
   the pre-scan from 'fclosure' + 'put_locN', and read when a call is expanded. */
static int whale_loc_fn[WHALE_JIT_MAX_SLOTS];
/* Which locals hold an object built in this body - 'object' followed by a store to
   that local, the same shape as whale_loc_fn above. Filled by the pre-scan (which
   already walks every opcode and already watches for exactly this pattern for
   fclosure) and read when the object is created, because whether it can be unboxed
   depends on the field accesses that come later. */
static uint8_t whale_loc_obj[WHALE_JIT_MAX_SLOTS];
static int whale_obj_pending;
static int whale_slot_fn[WHALE_JIT_MAX_SLOTS];
static int whale_vserial;                     /* next unused name number */

/* Everything encoded from now on goes into the workspace. */
static void fold_begin(void)
{
    JS_WhaleTokSink(gw_tok, gw_num, WHALE_TOK_MAX);
    go_len = 0;
    /* start a compile with no loop open and the output going to the stream */
    whale_loop_depth = 0;
    whale_out_tok = go_tok;
    whale_out_num = go_num;
    whale_out_len = &go_len;
    /* fresh name space for this function's materialised slots */
    whale_vserial = 0;
    whale_empty_ranges = 0;

    memset(whale_vname, 0, sizeof(whale_vname));
}

static int fold_mark(void)
{
    return JS_WhaleTokSinkLen();
}

/* A value whose expression grows past this many tokens is materialised right
   away. Without a ceiling, inlining a value used many times (a comparison
   inlined three times, say) could grow the function exponentially. */
#define WHALE_EXPR_MAX 24

static void fold_materialize(int k);
static void fold_commit(int mark);

/* Record [mark, now) as the value of stack slot k. */
static void fold_set(int k, int mark)
{
    whale_vexpr[k].off = mark;
    whale_vexpr[k].len = JS_WhaleTokSinkLen() - mark;
    whale_vmat[k] = 0;
    if (whale_vexpr[k].len > WHALE_EXPR_MAX)
        fold_materialize(k);
}

/* Append an expression range to what is being built right now.

   Always parenthesised: a folded value can be a comparison or a `?:`, and
   precedence then matters - `if (x < y ? 1 : 0 == 0)` parses as
   `x < y ? 1 : (0 == 0)`, i.e. always true, so the loop never runs. */
static void fold_inline_expr(const WhaleVExpr *e)
{
    int i;

    wt('(');
    for (i = 0; i < e->len; i++)
        JS_WhaleTokPush(gw_tok[e->off + i], gw_num[e->off + i]);
    wt(')');
}

/* Append the value of slot k to what is being built right now. */
static void fold_inline(int k)
{
    if (getenv("WHALE_EMPTY_DBG") && !whale_vmat[k] && whale_vexpr[k].len == 0)
        fprintf(stderr, "[empty] slot=%d (len=0, off=%d)\n", k, whale_vexpr[k].off);
    if (whale_vmat[k]) {
        wt_arr("v", whale_vname[k]);   /* already a real variable */
        return;
    }
    fold_inline_expr(&whale_vexpr[k]);
}

/* Emit ToInt32 of slot k. The helper is only needed for the modulo-2^32 wrap:
   a value already in the int32 range just needs a truncating cast, and that is
   the common case here (loop counters, small integers). So the range test is
   inlined and the call stays on the cold branch.
   
   Repeating the operand is what makes this pay: it is done only when the value
   is a plain variable or a short expression, which is exactly the shape the
   bitwise operators see. Without this, every bitwise operand paid a helper
   call whose fmod() dominated the loop. */
static void fold_toint32(int k, int is_i32, int is_iconst)
{
    int repeat;

    /* A literal already in range needs the cast and nothing else - no range
       test, no helper, and about fifteen tokens less than the general form.
       Bitwise code is full of these. */
    if (is_iconst) {
        if (!whale_vmat[k]) {
            if (whale_vexpr[k].len == 1) {
                wt('('); wk(JS_WK_INT); wt(')');
                wt_int((int)gw_num[whale_vexpr[k].off]);
                return;
            }
        }
    }

    /* Already an in-range int32: the range test and the helper call both fall
       away, leaving a plain cast. Only when the operand is already a variable -
       inlining an un-materialised operand copies a whole pending expression,
       which measured slower than the helper it replaced. */
    repeat = whale_vmat[k] || whale_vexpr[k].len <= 8;
    if (is_i32 && repeat) {
        wt('('); wk(JS_WK_INT); wt(')'); wt('('); fold_inline(k); wt(')');
        return;
    }

    if (!repeat) {
        wt_id("JS_WhaleToInt32"); wt('('); fold_inline(k); wt(')');
        return;
    }
    /* One conversion, no test and no helper. (int32_t)(long long)d is exactly
       ToInt32(d) for every double: within 2^63 long long holds it, so the cast
       truncates and the low 32 bits are the modulo-2^32 wrap; outside that range,
       and for NaN and the infinities, x86's cvttsd2si yields INT64_MIN, whose low
       32 bits are 0 - which is what ToInt32 gives for NaN, +/-Inf, and for any
       float large enough to be a multiple of 2^32 (everything past 2^63 is).
       The previous form tested the range and fell back to JS_WhaleToInt32 - which
       calls fmod - so it cost three comparisons and a branch per bitwise operand.
       Normal arithmetic just emits `+` and pays none of this, which is the whole
       reason a bitwise loop ran 7x slower than the same loop with arithmetic. */
    wt('('); wk(JS_WK_INT); wt(')'); wt('('); wk(JS_WK_LONG); wk(JS_WK_LONG); wt(')');
    wt('('); fold_inline(k); wt(')');
}

/* Emit `a % b`. fmod() is a libm call and the operands here are usually small
   positive integers, where the C remainder is exactly the same value, so test
   that first and keep fmod() on the cold branch.
   
   Only strictly positive integers take the fast path. Zero and -0 are left to
   fmod() on purpose: a truncating cast turns -0 into +0, and `fmod(-0, n)` is
   -0 while `1/x` downstream would notice the difference. */
/* Resolve property `atom` of the object in slot `os` through cache pair `fc`,
   storing the result in `v<nm>`. Both the slot and the atom are compile-time
   constants, so the shape check is the whole cost once the shape settles - the
   resolution only runs again if a property is added or removed. Reading the
   value itself is inlined: a helper call per access lands at interpreter speed,
   which is where the first version of this sat. */
static void fold_obj_field_load(int os, int atom, int fc, int nm, int stable)
{
    int fm = fold_mark();

    /* `stable` means the shape cannot change for the rest of the function: the
       object cannot escape (no calls) and every store targets a field the
       literal already defined. The resolution then happens once and the shape
       check disappears entirely, which is the difference between beating the
       interpreter and merely matching it. */
    if (stable) {
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fs", fc); wt_op(JS_OP_OR);
        wt_arr("fv", fc); wt_op(JS_OP_NE); wt_id("_ws"); wt('.'); wt_id("fver"); wt(')'); wt('{');
    } else {
        wk(JS_WK_IF); wt('('); wt_arr("fv", fc); wt_op(JS_OP_NE); wt_id("_ws"); wt('.'); wt_id("fver");
        wt_op(JS_OP_OR); wt_arr("fs", fc); wt_op(JS_OP_NE);
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
        wt('{');
    }
    wt_arr("fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc); wt(')'); wt(';');
    wt_arr("fv", fc); wt('='); wt_id("_ws"); wt('.'); wt_id("fver"); wt(';');
    wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fp", fc); wt(')'); wt('{');
    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    wt('}');
    /* int32 or float64 inline; anything else hands off to the cold helper.
       Assignment, not a declaration: the prologue already declared every v<n>. */
    wt_arr("v", nm); wt('=');
    wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
    wt('?'); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
    wt(':'); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
    wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
    wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(','); wt_arr("fp", fc); wt(','); wt_id("deopt"); wt(')');
    wt(';');
    fold_commit(fm);
}

/* Resolve `atom` of the object in slot `os` through cache pair `fc` without
   loading anything, so a store can follow. Emits a deopt when the value it
   would overwrite is not a number: overwriting a reference without freeing it
   would leak. */
static void fold_obj_field_resolve(int os, int atom, int fc, int stable)
{
    int fm = fold_mark();

    if (stable) {
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fs", fc); wt_op(JS_OP_OR);
        wt_arr("fv", fc); wt_op(JS_OP_NE); wt_id("_ws"); wt('.'); wt_id("fver"); wt(')'); wt('{');
    } else {
        wk(JS_WK_IF); wt('('); wt_arr("fv", fc); wt_op(JS_OP_NE); wt_id("_ws"); wt('.'); wt_id("fver");
        wt_op(JS_OP_OR); wt_arr("fs", fc); wt_op(JS_OP_NE);
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
        wt('{');
    }
    wt_arr("fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc); wt(')'); wt(';');
    wt_arr("fv", fc); wt('='); wt_id("_ws"); wt('.'); wt_id("fver"); wt(';');
    wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fp", fc); wt(')'); wt('{');
    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    wt('}');
    wk(JS_WK_IF); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(8); wt(')');
    wt('{'); wk(JS_WK_IF); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(0); wt(')');
    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}'); wt('}');
    fold_commit(fm);
}

static void fold_mod(int ka, int kb)
{

    int repeat = (whale_vmat[ka] || whale_vexpr[ka].len <= 8) &&
                 (whale_vmat[kb] || whale_vexpr[kb].len <= 8);

    if (!repeat) {
        wt_id("fmod"); wt('('); fold_inline(ka); wt(','); fold_inline(kb); wt(')');
        return;
    }
    wt('(');
    /* cheap range tests first, the integrality test last */
    wt('('); fold_inline(ka); wt_op(JS_OP_GT); wt_int(0); wt(')'); wt_op(JS_OP_AND);
    wt('('); fold_inline(kb); wt_op(JS_OP_GT); wt_int(0); wt(')'); wt_op(JS_OP_AND);
    wt('('); fold_inline(ka); wt_op(JS_OP_LE); wt_num(2147483647.0); wt(')'); wt_op(JS_OP_AND);
    wt('('); fold_inline(kb); wt_op(JS_OP_LE); wt_num(2147483647.0); wt(')'); wt_op(JS_OP_AND);
    wt('('); fold_inline(ka); wt_op(JS_OP_EQ); wt('('); wk(JS_WK_INT); wt(')'); fold_inline(ka); wt(')'); wt_op(JS_OP_AND);
    wt('('); fold_inline(kb); wt_op(JS_OP_EQ); wt('('); wk(JS_WK_INT); wt(')'); fold_inline(kb); wt(')');
    wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('(');
    wt('('); wk(JS_WK_INT); wt(')'); fold_inline(ka); wt('%'); wt('('); wk(JS_WK_INT); wt(')'); fold_inline(kb);
    wt(')');
    wt(':'); wt_id("fmod"); wt('('); fold_inline(ka); wt(','); fold_inline(kb); wt(')');
    wt(')');
}

/* Copy the token range [mark, now) to the current output. */
static void fold_commit(int mark)
{
    int i, n = JS_WhaleTokSinkLen();

    for (i = mark; i < n && *whale_out_len < WHALE_TOK_MAX; i++) {
        whale_out_num[*whale_out_len] = gw_num[i];
        whale_out_tok[(*whale_out_len)++] = gw_tok[i];
    }
}

/* WhaleJS: the loop buffer is OFF.
 *
 * It was meant to hand a complete loop body to a loop-level pass, and no such
 * pass was ever written - so all it did was cost memory and break silently:
 * a body larger than WHALE_LOOP_BUF (4096) lost its tail, and the bound used
 * when appending was WHALE_TOK_MAX (32768), so the same overflow wrote past the
 * end of l->tok. Making loop bodies longer (which is what handing instructions
 * to the engine does) exposes both at once.
 *
 * Returning 0 is the "stack is full" path this function already had: the body
 * is emitted straight through into go_tok, which is the same bound the whole
 * function is held to. whale_loop_depth stays 0, so loop_end() is never
 * entered. Nothing is lost - no loop-level pass exists to receive the body. */
static int loop_begin(int head_pos, int back_pos)
{
    (void)head_pos;
    (void)back_pos;
    return 0;
}

/* Finish the innermost loop and hand its body on. This is where a loop-level
   optimisation would run - it gets the complete body at once. */
static void loop_end(void)
{
    WhaleLoop *l, *p;
    int i, n;

    if (whale_loop_depth <= 0)
        return;
    whale_loop_depth--;
    l = &whale_loops[whale_loop_depth];

    /* switch back to the enclosing output target first, then append the body */
    if (whale_loop_depth == 0) {
        whale_out_tok = go_tok;
        whale_out_num = go_num;
        whale_out_len = &go_len;
    } else {
        p = &whale_loops[whale_loop_depth - 1];
        whale_out_tok = p->tok;
        whale_out_num = p->num;
        whale_out_len = &p->len;
    }
    n = l->len;
    if (n > WHALE_LOOP_BUF)
        n = WHALE_LOOP_BUF;
    for (i = 0; i < n && *whale_out_len < WHALE_TOK_MAX; i++) {
        whale_out_num[*whale_out_len] = l->num[i];
        whale_out_tok[(*whale_out_len)++] = l->tok[i];
    }
}

/* Force slot k into a variable (`v<k> = <expr>;`) and commit it. Only needed
   where a value has to survive a basic-block boundary or a jump. */
static void fold_materialize(int k)
{
    int mark;

    if (whale_vmat[k])
        return;
    /* Take a fresh name: whatever inlined this slot beforehand recorded the old
       name as a token, and rewriting that variable would silently change those
       references. The prologue declares stack_size + WHALE_VSER_MAX variables,
       so this cannot run out for any function seen so far. */
    whale_vname[k] = whale_vserial++;
    mark = fold_mark();
    wt_arr("v", whale_vname[k]); wt('=');
    fold_inline(k);
    wt(';');
    fold_commit(mark);
    whale_vmat[k] = 1;
}

/* ---- deferred locals -----------------------------------------------------
 * A write to a local inside a basic block does not have to become a store right
 * away. If the value is read once in the same block, keeping it in the
 * expression chain removes a store/load pair; statement count is what dominates
 * here, because TinyCC spills at every statement boundary. The second read, or
 * the end of the block, materialises it into the real variable.
 */

/* Turn the pending expression of local d into a real store. */
static void local_materialize(int d)
{
    WhaleLocal *m = &whale_lmem[d];
    int mark;

    if (!m->live)
        return;
    if (getenv("WHALE_MEM_TRACE"))
        fprintf(stderr, "[mat] L%d off=%d len=%d\n", d, m->expr.off, m->expr.len);
    mark = fold_mark();
    wt_slot("L", d); wt('=');
    fold_inline_expr(&m->expr);
    wt(';');
    fold_commit(mark);
    m->live = 0;
    m->used = 0;
}

/* Emit the value of local d: the pending expression if there is one, else the
   variable.

   Always inlines, even on a repeat read. Materialising here would be wrong when
   it happens in the middle of building another expression: the store would be
   emitted before the expression that consumes it, turning `a + b` into `a; b`.
   Duplicating the expression is safe because it is pure, and the length ceiling
   in local_set() keeps the duplication bounded. */
static void local_inline(int d)
{
    if (getenv("WHALE_MEM_TRACE"))
        fprintf(stderr, "[inl] L%d live=%d\n", d, whale_lmem[d].live);
    if (!whale_lmem[d].live) {
        wt_slot("L", d);
        return;
    }
    fold_inline_expr(&whale_lmem[d].expr);
}

/* Forget any pending expression for d, without emitting it. Used when every
   live local has to be written out (a basic-block boundary). */
static void local_forget(int d)
{
    whale_lmem[d].live = 0;
    whale_lmem[d].used = 0;
}

/* Record the expression just built in the workspace as the pending value of
   local d. Falls back to storing it when it grew too long to duplicate. */
static void local_set(int d, int mark)
{
    WhaleLocal *m = &whale_lmem[d];

    m->expr.off = mark;
    m->expr.len = JS_WhaleTokSinkLen() - mark;
    m->live = 1;
    m->used = 0;
    if (getenv("WHALE_MEM_TRACE"))
        fprintf(stderr, "[set] L%d off=%d len=%d\n", d, m->expr.off, m->expr.len);
    if (m->expr.len > WHALE_EXPR_MAX)
        local_materialize(d);
}

/* Materialise every deferred local. Called at a basic-block boundary, where the
   values have to be readable from the variables again. */
static void local_flush_all(int n)
{
    int d;

    for (d = 0; d < n; d++)
        local_materialize(d);
}

/* Generate + type-check in one pass. Returns 0 when the function is not a
   candidate (unsupported opcode, unknown type, inconsistent jump target). */
/* ---------------------------------------------------------------------------
 * Inlining a callee the generator can see.
 *
 * NOT WIRED UP - none of this ever fires. The recogniser is correct and the
 * pre-scan does record the declaration (verified: "[prescan] pos=0 m=0 ln=0
 * var_count=4"), but the caller's compilation ends before its main loop reads
 * that local, so WHALE_T_FN is never produced and no call site expands. It ends
 * without reporting a bail, which is why it is worth another look. Until then
 * this is inert: 24 perftest cases, checks identical to node.
 *
 * What IS live from this work: a function declaration emits nothing, so a
 * nested unction no longer makes its enclosing body uncompilable (it used to
 * bail at OP_fclosure8).
 *
 * A function declared inside this very body reaches its call sites through a
 * local, and `push_const M; fclosure; put_locN` is what puts it there. That local
 * is a compile-time fact, so the call can be expanded instead of going through
 * the engine - and that is the whole difference: this shape spends ~190ms per
 * 10^6 calls on the crossing alone, while the expanded form runs at node speed.
 *
 * The expansion does not invent a stack. The callee's arguments are the caller's
 * operand slots at arg_base..arg_base+arg_count-1 (already evaluated), its
 * temporaries use the caller's stack above that point, and its result is left in
 * caller slot arg_base (the callee's result slot). Object slots and field-cache
 * entries are shared, which is safe because one linear pass hands them out from
 * the same counters.
 *
 * Returns the type of the inlined result, or -1 if the callee is out of shape.
 * v1 accepts what the front-end helpers actually look like: no locals of its own,
 * straight line, one `return <expression>`.
 * ------------------------------------------------------------------------- */
static int whale_inline_argmap[4];   /* inlined callee's arg k -> caller stack slot */
static int whale_inline_nested;      /* one level at a time: see the call site */
/* Declared again further down with the generator's own state. The inliner hands
   out slots from both, which is safe because a single linear pass allocates from
   them - the two areas cannot collide. */
int whale_objs;
int whale_fields;
/* Defined further down with the rest of the generator state. The inliner hands
   out slots from both, which is safe because a single linear pass allocates from
   them - the two areas cannot collide. */
int whale_objs;
int whale_fields;

static int whale_inline_body(const JSWhaleFuncInfo *callee, uint8_t *cur,
                             int var_count, int arg_base)
{
    const uint8_t *pc = callee->byte_code;
    int len = callee->byte_code_len;
    int pos, sp = 0, i, ret = -1;
    int saved_argmap[4];

    if (callee->var_count != 0)
        return -1;                      /* locals would need slot remapping */
    if (callee->arg_count > 4)
        return -1;
    for (i = 0; i < 4; i++) {
        saved_argmap[i] = whale_inline_argmap[i];
        whale_inline_argmap[i] = (i < callee->arg_count) ? arg_base + i : -1;
    }

    for (pos = 0; pos < len; ) {
        int op = (int)pc[pos];
        int sz = whale_op_size_of(op);
        int ta, tb, fm;

        if (sz <= 0 || pos + sz > len)
            goto done;
        switch (op) {
        case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
        case OP_get_arg:
            {
                int a = (op == OP_get_arg) ? (int)whale_get_u16(pc + pos + 1) : op - OP_get_arg0;

                if (a < 0 || a >= callee->arg_count || whale_inline_argmap[a] < 0)
                    goto done;
                fm = fold_mark();
                fold_inline(whale_inline_argmap[a]);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;

        case OP_object:
            {
                int os;

                if (whale_objs >= WHALE_CALL_BASE)
                    goto done;
                os = whale_objs++;
                fm = fold_mark();
                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjNew"); wt('(');
                wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
                wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                fold_commit(fm);
                cur[var_count + sp] = WHALE_OBJ_T(os);
                sp++;
            }
            break;

        case OP_define_field:
            {
                int atom = (int)whale_get_u32(pc + pos + 1);

                if (sp < 2)
                    goto done;
                ta = cur[var_count + sp - 2];
                tb = cur[var_count + sp - 1];
                if (!WHALE_IS_OBJ(ta) || !whale_type_is_num(tb))
                    goto done;
                fm = fold_mark();
                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum"); wt('(');
                wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(','); wt_int(atom); wt(',');
                fold_inline(sp - 1); wt(')'); wt(')');
                wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                fold_commit(fm);
                sp--;
            }
            break;

        case OP_get_field:
            {
                int atom = (int)whale_get_u32(pc + pos + 1);
                int fc, nm;

                if (sp < 1)
                    goto done;
                ta = cur[var_count + sp - 1];
                if (!WHALE_IS_SLOT(ta) || whale_fields >= WHALE_FIELD_CACHE)
                    goto done;
                fc = whale_fields++;
                nm = whale_vserial++;
                /* not a literal we can prove stable: keep the shape check */
                fold_obj_field_load(WHALE_SLOT(ta), atom, fc, nm, 0);
                whale_vname[sp - 1] = nm;
                whale_vmat[sp - 1] = 1;
                cur[var_count + sp - 1] = WHALE_T_NUM;
            }
            break;

        case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
            {
                fm = fold_mark();
                wt_int(op - OP_push_0);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;

/* Hand a binary operation to the engine when one operand is opaque.

   The generator refuses an operator as soon as a side is not a number, and that one
   refusal used to cost the whole function - a vnode record keeps a string key, so it
   never compiled at all. The engine has the real semantics in its js_*_slow
   functions; the values are already parked in slots, so all that is missing is the
   call. Numeric sides are materialised into a slot first, then the operation runs and
   the result is parked, keeping the value opaque (WHALE_T_VAL) as it should be.

   A macro rather than a function because whale_objs, cur and var_count are locals of
   whale_jit_gen. `ok` is set to 0 when there is no room to park the values, and the
   caller then falls back to giving up. */
#define WHALE_EMIT_BINOP_VAL(op, ta, tb, ia, ib, ok)                      \
    do {                                                                  \
        int _sa, _sb, _os, _fm;                                           \
        ok = 0;                                                           \
        if (whale_objs + 3 <= WHALE_CALL_BASE) {                          \
            _sa = WHALE_IS_VAL(ta) ? WHALE_VAL_SLOT(ta) : whale_objs++;   \
            _sb = WHALE_IS_VAL(tb) ? WHALE_VAL_SLOT(tb) : whale_objs++;   \
            _os = whale_objs++;                                           \
            _fm = fold_mark();                                            \
            if (!WHALE_IS_VAL(ta)) {                                      \
                wt_id("JS_WhaleObjSlotNum"); wt('('); wt_id("ctx");       \
                wt(','); wt_int(_sa); wt(',');                            \
                fold_inline(ia); wt(')'); wt(';');                        \
            }                                                             \
            if (!WHALE_IS_VAL(tb)) {                                      \
                wt_id("JS_WhaleObjSlotNum"); wt('('); wt_id("ctx");       \
                wt(','); wt_int(_sb); wt(',');                            \
                fold_inline(ib); wt(')'); wt(';');                        \
            }                                                             \
            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleBinopVal");    \
            wt('('); wt_id("ctx"); wt(','); wt_int(op); wt(',');          \
            wt_int(_sa); wt(','); wt_int(_sb); wt(','); wt_int(_os);      \
            wt(')'); wt(')'); wt('{');                                    \
            wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');         \
            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');                \
            fold_commit(_fm);                                             \
            cur[var_count + ia] = WHALE_VAL_T(_os);                       \
            ok = 1;                                                       \
        }                                                                 \
    } while (0)

        case OP_add: case OP_sub: case OP_mul: case OP_div:
            {
                int opid = (op == OP_add) ? JS_OP_ADD : (op == OP_sub) ? JS_OP_SUB :
                           (op == OP_mul) ? JS_OP_MUL : JS_OP_DIV;

                if (sp < 2)
                    goto done;
                ta = cur[var_count + sp - 2];
                tb = cur[var_count + sp - 1];
                if (!whale_type_is_num(ta) || !whale_type_is_num(tb))
                    goto done;
                fm = fold_mark();
                fold_inline(sp - 2); wt_op(opid); fold_inline(sp - 1);
                fold_set(sp - 2, fm);
                cur[var_count + sp - 2] = WHALE_T_NUM;
                sp--;
            }
            break;

        case OP_return:
            {
                if (sp < 1)
                    goto done;
                ta = cur[var_count + sp - 1];
                fm = fold_mark();
                if (WHALE_IS_SLOT(ta)) {
                    /* An object result cannot live in a double slot, so park it in
                       one of its own - the caller then sees a slot value, which is
                       what it would have seen from the compiled form. */
                    int os = whale_objs++;

                    if (os >= WHALE_CALL_BASE)
                        goto done;
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjSlotCopy"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(WHALE_SLOT(ta)); wt(','); wt_int(os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    ret = WHALE_VAL_T(os);
                } else if (whale_type_is_num(ta)) {
                    int nm = whale_vserial++;

                    wt_arr("v", nm); wt('='); fold_inline(sp - 1); wt(';');
                    whale_vname[sp - 1] = nm;
                    whale_vmat[sp - 1] = 1;
                    ret = WHALE_T_NUM;
                } else {
                    goto done;
                }
                fold_commit(fm);
                cur[var_count + sp - 1] = (uint8_t)ret;
                goto done;
            }

        default:
            goto done;
        }
        pos += sz;
    }

  done:
    for (i = 0; i < 4; i++)
        whale_inline_argmap[i] = saved_argmap[i];
    return ret;
}
/* The observations for the function being generated: the generator compiles one
   function at a time, so this does not need to travel through every call. */
static const void *whale_prof_now;

/* Did this site hold exactly one shape the whole way through the warm-up?

   That is the evidence the static analysis cannot produce. `whale_shape_stable`
   requires the function to make no calls at all, so a record built by a call -
   `var o = make()` - fails it even when the shape never moves. And a failing site is
   not merely a missed optimisation: the generated condition is
   `fv[fc] != fver || fs[fc] != JS_WhaleObjShape(...)`, and _ws.fver is a global
   version bumped by ANY shape change, so a function that creates an object per
   iteration re-resolves on nearly every access.

   One observed shape says the site behaved as a constant for a hundred calls. It is
   an observation, not a proof - a shape could still change later - but the general
   form is what handles that, and it is what runs when this returns false. */
/* The union of the field atoms seen across this site's observed shapes, in a stable
   order, or 0 when there is nothing to lay out.

   The union rather than a single shape, because reserving a slot is cheap and missing
   one is not: a field that only some of the observed shapes carry still gets its own
   double and a presence flag, which costs eight bytes and keeps the access native
   instead of dropping to a shape check plus a heap chase. That also covers the case
   where the same variable is assigned quite different objects down different
   branches - the layout simply covers both.

   Fields are matched by atom, so the same property reached through two shapes is one
   slot. `atoms` is filled in first-seen order, which is stable for a given
   observation and therefore gives a fixed layout. */
static int whale_struct_fields(unsigned off, int *atoms, int max)
{
    const void *shapes[8];
    int n, i, j, k = 0;

    if (off == 0 || whale_prof_now == NULL || max <= 0)
        return 0;
    n = JS_WhaleProfShapes(whale_prof_now, off, shapes, 8);
    for (i = 0; i < n; i++) {
        int fa[WHALE_STRUCT_FIELDS];
        int fn = JS_WhaleShapeFields(shapes[i], fa, WHALE_STRUCT_FIELDS);

        for (j = 0; j < fn; j++) {
            int seen = 0, m;

            for (m = 0; m < k; m++)
                if (atoms[m] == fa[j]) { seen = 1; break; }
            if (!seen && k < max)
                atoms[k++] = fa[j];
        }
    }
    return k;
}

/* The column a field atom occupies in a layout, or -1 when it is not in it.
   Takes the layout explicitly because the layout itself lives in whale_jit_gen's
   frame (whale_str_atoms / whale_str_n). */
static int whale_str_col(const int *atoms, int n, int atom)
{
    int i;

    for (i = 0; i < n; i++)
        if (atoms[i] == atom)
            return i;
    return -1;
}

static int whale_prof_one_shape(unsigned off)
{
    /* 8 >= the engine cap on shapes per site, so the count it returns is exact */
    const void *sh[8];

    {
        int n = (off != 0 && whale_prof_now != NULL)
            ? JS_WhaleProfShapes(whale_prof_now, off, sh, 8) : 0;

        /* negative means the site was observed and given up on - treat it as "not
           monomorphic", which is what the caller is asking */
        if (n < 0)
            n = 2;
        static int dbg;

        if (getenv("WHALE_PROF_DBG") && dbg < 40) {
            int fa[WHALE_STRUCT_FIELDS];
            int fn = whale_struct_fields(off, fa, WHALE_STRUCT_FIELDS);
            int q;

            dbg++;
            fprintf(stderr, "[prof] lookup off=%u prof=%p -> n=%d%s fields=%d [",
                    off, whale_prof_now, n, (n == 1 ? " STABLE" : ""), fn);
            for (q = 0; q < fn; q++)
                fprintf(stderr, "%s%d", q ? "," : "", fa[q]);
            fprintf(stderr, "]\n");
        }
        return n == 1;
    }
}

/* Is this field access site safe to build a fixed layout on?

   Three answers, not two. A site the warm-up never reached returns 0 from the engine
   and is accepted here - its silence says nothing about the shape, and refusing it
   would discard objects that are in fact stable. A site observed holding one shape is
   accepted. A site observed holding several, or given up on by the recorder, comes
   back negative and is refused outright: that is the one case where the observation
   actively says the layout is not fixed. */
static int whale_prof_site_ok(unsigned off)
{
    const void *sh[8];
    int n;

    if (off == 0 || whale_prof_now == NULL)
        return 0;
    n = JS_WhaleProfShapes(whale_prof_now, off, sh, 8);
    if (getenv("WHALE_PROF_DBG"))
        fprintf(stderr, "[site] off=%u n=%d %s\n", off, n,
                (n == 0 || n == 1) ? "ok" : "REFUSED");
    return n == 0 || n == 1;
}

/* =========================================================================
 * The general fallback: hand one instruction to the engine
 *
 * `goto out` abandons the whole function, which makes every new type or opcode
 * an all-or-nothing change. This is the alternative: push this instruction's
 * operands onto the frame's own operand stack, let the engine run that one
 * instruction (the JS_WhaleStep* services), and take the results back into the
 * double domain. The value representation stays what it is - only the handful
 * of values this instruction touches are converted.
 *
 * Returns 1 when the instruction was emitted and `*sp_io` advanced, 0 when it
 * cannot be. The caller's answer to 0 stays `goto out`: this widens what can be
 * lowered, it does not promise anything.
 *
 * 0 comes back when the opcode is not covered, when an operand is something the
 * operand stack cannot carry (an unboxed struct, or a value whose type the
 * generator lost track of), when the extra variables would not fit, and for
 * anything that is control flow - a branch the generated code did not emit
 * itself is not one it can follow.
 *
 * A result that is not a number deoptimises the call rather than being tracked
 * as an opaque value. The point of the fallback is to keep the function
 * compiling; a surprising type still belongs to the interpreter.
 * ========================================================================= */
static int whale_step_is_control(int op)
{
    switch (op) {
    case OP_goto: case OP_goto8: case OP_goto16:
    case OP_if_false: case OP_if_false8:
    case OP_if_true: case OP_if_true8:
    case OP_gosub: case OP_ret:
    case OP_return: case OP_return_undef: case OP_return_async:
    case OP_throw: case OP_throw_error:
    case OP_catch: case OP_nip_catch:
    case OP_call: case OP_call0: case OP_call1: case OP_call2: case OP_call3:
    case OP_tail_call: case OP_tail_call_method:
    case OP_call_method: case OP_array_from: case OP_apply:
    case OP_call_constructor: case OP_iterator_call: case OP_apply_eval:
        return 1;
    default:
        return 0;
    }
}

static int whale_step_emit(int op, int pos, int *sp_io, uint8_t *cur, int var_count,
                           int stack_size)
{
    int n_pop = 0, n_push = 0, i, sp = *sp_io, fm;
    int dbg = getenv("WHALE_STEP_DBG") != NULL;

    /* Bisect switch: WHALE_STEP_OFF=<opcode> refuses that one opcode, so a single
       hand-over can be turned off without touching the rest. It is how "object in
       a value position" was isolated, and it stays for the next one. */
    {
        const char *off = getenv("WHALE_STEP_OFF");

        if (off && atoi(off) == op)
            return 0;
    }

    if (whale_step_is_control(op) || !JS_WhaleStepCovers(op)) {
        if (dbg)
            fprintf(stderr, "[emit] pos=%d op=%d REFUSE not-covered\n", pos, op);
        return 0;
    }
    JS_WhaleOpArity(op, &n_pop, &n_push);
    if (n_pop < 0 || n_push < 0 || n_pop > sp || n_push > 8) {
        if (dbg)
            fprintf(stderr, "[emit] pos=%d op=%d REFUSE arity np=%d nu=%d sp=%d\n",
                    pos, op, n_pop, n_push, sp);
        return 0;
    }
    for (i = 0; i < n_pop; i++) {
        int t = cur[var_count + sp - n_pop + i];

        /* Operand 0 of a property access is its receiver, and a parked object is
           exactly what that wants to be. Every other operand has to be a number:
           a parked object in a value position is what makes the generated code
           come out wrong - `out[i] = t` crashes where `out[i] = 3` is fine, with
           no difference visible on the engine side - so those keep `goto out`
           until the cause is found. Object unboxing takes the object out of this
           position anyway, which is why it is the right next step and not a
           detour. See notes/whale-step-repro.md. */
        if (i > 0 && !whale_type_is_num(t)) {
            if (dbg)
                fprintf(stderr, "[emit] pos=%d op=%d REFUSE object in value "
                        "position slot=%d t=%d\n", pos, op, sp - n_pop + i, t);
            return 0;
        }
        if (!whale_type_is_num(t) && !WHALE_IS_SLOT(t)) {
            if (dbg)
                fprintf(stderr, "[emit] pos=%d op=%d REFUSE type slot=%d t=%d\n",
                        pos, op, sp - n_pop + i, t);
            return 0;
        }
    }
    /* room for the variables this names: one per operand that still needs a
       materialisation, one per result */
    if (whale_vserial + n_pop + n_push >= stack_size + WHALE_VSER_MAX) {
        if (dbg)
            fprintf(stderr, "[emit] pos=%d op=%d REFUSE vserial %d\n", pos, op,
                    whale_vserial);
        return 0;
    }

    fm = fold_mark();
    /* The numeric operands have to be in variables before the pushes are
       emitted: fold_materialize emits a statement of its own, which cannot land
       in the middle of a call. */
    for (i = 0; i < n_pop; i++) {
        int k = sp - n_pop + i;

        if (whale_type_is_num(cur[var_count + k]))
            fold_materialize(k);
    }
    for (i = 0; i < n_pop; i++) {
        int k = sp - n_pop + i;
        int t = cur[var_count + k];

        wk(JS_WK_IF); wt('('); wt('!');
        if (whale_type_is_num(t)) {
            wt_id("JS_WhaleStepPushNum"); wt('('); wt_id("ctx"); wt(','); wt_int(i);
            wt(','); fold_inline(k); wt(')');
        } else {
            wt_id("JS_WhaleStepPushSlot"); wt('('); wt_id("ctx"); wt(','); wt_int(i);
            wt(','); wt_int(WHALE_SLOT(t)); wt(')');
        }
        wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
        wt_id("JS_WhaleStepClear"); wt('('); wt_id("ctx"); wt(','); wt_int(n_pop); wt(')'); wt(';');
        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    }
    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStepRun"); wt('('); wt_id("ctx");
    wt(','); wt_int(pos); wt(','); wt_int(n_pop); wt(','); wt_int(n_push); wt(')'); wt(')');
    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
    wt_id("JS_WhaleStepClear"); wt('('); wt_id("ctx"); wt(','); wt_int(n_pop); wt(')'); wt(';');
    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    for (i = 0; i < n_push; i++) {
        int k = sp - n_pop + i, nm;

        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStepOutIsNum"); wt('(');
        wt_id("ctx"); wt(','); wt_int(i); wt(')'); wt(')'); wt('{');
        wt_id("JS_WhaleStepClear"); wt('('); wt_id("ctx"); wt(','); wt_int(n_push); wt(')'); wt(';');
        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
        nm = whale_vserial++;
        wt_arr("v", nm); wt('='); wt_id("JS_WhaleStepOutNum"); wt('('); wt_id("ctx");
        wt(','); wt_int(i); wt(')'); wt(';');
        whale_vname[k] = nm;
        whale_vmat[k] = 1;   /* it is a real variable now */
        cur[var_count + k] = WHALE_T_NUM;
    }
    if (n_push > 0) {
        wt_id("JS_WhaleStepClear"); wt('('); wt_id("ctx"); wt(','); wt_int(n_push); wt(')'); wt(';');
    }
    fold_commit(fm);
    sp += n_push - n_pop;
    *sp_io = sp;
    return 1;
}

static int whale_jit_gen(JSContext *ctx, const JSWhaleFuncInfo *info)
{
    whale_prof_now = info->prof;
    /* What JS_UNDEFINED actually looks like, measured rather than assumed. The tag
       value, the word boundaries and the width are all things to read off the engine,
       and guessing them has already cost several rounds. */
    if (getenv("WHALE_UNDEF_DBG")) {
        static int once;
        union { JSValue v; unsigned char b[16]; } u;
        int q;

        if (!once) {
            once = 1;
            memset(&u, 0, sizeof u);
            u.v = JS_UNDEFINED;
            fprintf(stderr, "[undef] sizeof(JSValue)=%d bytes:", (int)sizeof(JSValue));
            for (q = 0; q < (int)sizeof(JSValue) && q < 16; q++)
                fprintf(stderr, " %02x", u.b[q]);
            fprintf(stderr, "  <- JS_UNDEFINED\n");
            memset(&u, 0, sizeof u);
            u.v = JS_NULL;
            fprintf(stderr, "[undef]                  ");
            for (q = 0; q < (int)sizeof(JSValue) && q < 16; q++)
                fprintf(stderr, " %02x", u.b[q]);
            fprintf(stderr, "  <- JS_NULL\n");
            memset(&u, 0, sizeof u);
            u.v = JS_MKVAL(JS_TAG_FLOAT64, 7);
            fprintf(stderr, "[undef]                  ");
            for (q = 0; q < (int)sizeof(JSValue) && q < 16; q++)
                fprintf(stderr, " %02x", u.b[q]);
            fprintf(stderr, "  <- a float64\n");
        }
    }
    /* WHALE_JIT_DUMP identifies a compilation by arg_count/var_count/byte length,
       which is enough to tell one JS function from another: in objfields, make() is
       0/0 and run() is 1/1. The generated names (whale_jit_fn_N) are sequence
       numbers and say nothing about which function they are. */
    if (getenv("WHALE_JIT_DUMP"))
        fprintf(stderr, "[gen] argc=%d varc=%d len=%d prof=%p\n",
                info->arg_count, info->var_count, info->byte_code_len, info->prof);
    whale_gen_self_ref = 0;
    {
        static int once;
        if (!once && getenv("WHALE_JIT_DUMP")) {
            once = 1;

            fprintf(stderr, "whale jit op ids: get_var_ref0=%d add=%d invalid=%d count=%d temp_start=%d temp_end=%d\n",
                    OP_get_var_ref0, OP_add, OP_invalid, OP_COUNT, OP_TEMP_START, OP_TEMP_END);
        }
    }
    const uint8_t *pc = info->byte_code;
    int len = info->byte_code_len;
    int var_count = info->var_count;
    int stack_size = info->stack_size;
    int arg_count = info->arg_count;
    int state_len = var_count + stack_size + 1; /* +1: sp */
    /* Before the pre-scan, which is what fills these in: clearing them later
       (fold_begin runs further down) would wipe what it found. */
    memset(whale_loc_fn, 0xff, sizeof(whale_loc_fn));
    memset(whale_slot_fn, 0xff, sizeof(whale_slot_fn));
    int pos, i, sp, ret = 0, tail_mark;
    uint8_t *targets = NULL, *seen = NULL, *snap = NULL, *cur = NULL, *args_used = NULL;
    uint8_t *args_obj = NULL;   /* per argument: read as the object of a field access */
    int *arg_obj_slot = NULL;   /* per argument: the slot it travels in, or -1 */
    int *loc_slot = NULL;       /* per local: its own slot when it is reassigned */
    int *loop_back = NULL;      /* per position: the backward jump that closes it */
    uint8_t var_ref_used[4] = { 0, 0, 0, 0 };
    unsigned *assign_cnt = NULL;   /* weighted access count */
    uint8_t *induction = NULL;     /* the local is a for-loop counter */
    uint8_t lo_c[WHALE_JIT_MAX_SLOTS];
    double lo_cv[WHALE_JIT_MAX_SLOTS];
    int unreachable = 0;
    int ret_is_bool = -1;
    int whale_objs = 0;   /* object slots handed out; each new object takes one */
    int whale_has_call = 0; /* a call clobbers registers, so no pinning then */
    int whale_fields = 0;   /* object-property cache pairs handed out */
    int whale_shape_stable = 0; /* object shapes cannot change after this point */
    int whale_ret_obj = 0;      /* the function can return an array or object */
    int whale_def_atoms[WHALE_ATOM_MAX];
    int whale_put_atoms[WHALE_ATOM_MAX];
    int whale_def_atoms_n = 0;
    int whale_put_atoms_n = 0;
    int whale_call_pos[WHALE_CALL_MAX];
    int whale_call_n = 0;
    /* every field access site in this function, as the offset the profile table is
       keyed by (opcode offset + 1). The candidate test is asked at the FUNCTION
       level - "was every field access here observed monomorphic" - rather than per
       object, because both cases with a measurement behind them build exactly one
       object, and per-object would need a provenance array kept in step with cur[]
       and snapshotted at every jump target. */
    int whale_field_sites[WHALE_ATOM_MAX];
    int whale_field_sites_n = 0;
    /* The layout of an unboxed struct: which atom sits in which column, plus the
       object slot index it was laid out for. Filled when OP_object creates one, read
       by every field access on that slot. */
    int whale_str_atoms[WHALE_STRUCT_FIELDS];
    int whale_str_n = 0;
    int whale_str_slot = -1;
    int whale_nloops = 0;   /* backward jumps; more than one means nesting */

    if (!info->is_normal)
        return 0;
    if (stack_size <= 0 || stack_size > WHALE_JIT_MAX_SLOTS)
        return 0;
    if (info->var_ref_count > 4)
        return 0;

    targets = calloc(1, len + 1);
    seen = calloc(1, len + 1);
    snap = calloc((size_t)(len + 1), state_len);
    cur = calloc(1, state_len);
    args_used = calloc(1, arg_count > 0 ? arg_count : 1);
    args_obj = calloc(1, arg_count > 0 ? arg_count : 1);
    arg_obj_slot = malloc(sizeof(int) * (arg_count > 0 ? arg_count : 1));
    loc_slot = malloc(sizeof(int) * (var_count > 0 ? var_count : 1));
    assign_cnt = calloc(var_count > 0 ? var_count : 1, sizeof(unsigned));
    induction = calloc(var_count > 0 ? var_count : 1, 1);
    whale_vexpr = calloc(stack_size, sizeof(WhaleVExpr));
    whale_vmat = calloc(1, stack_size);
    whale_lmem = calloc(var_count > 0 ? var_count : 1, sizeof(WhaleLocal));
    loop_back = malloc((len + 1) * sizeof(*loop_back));
    if (!targets || !seen || !snap || !cur || !args_used || !assign_cnt ||
        !whale_vexpr || !whale_vmat || !whale_lmem || !loop_back || !induction)
        goto out;
    for (i = 0; i <= len; i++)
        loop_back[i] = -1;
    memset(lo_c, 0, sizeof(lo_c));
    memset(lo_cv, 0, sizeof(lo_cv));

    /* pass 1: jump targets + which arguments are read */
    for (pos = 0; pos < len; ) {
        int op = pc[pos];
        int sz = whale_op_size_of(op);

        if (sz <= 0 || pos + sz > len)
            goto out;
        if (op == OP_goto8 || op == OP_if_false8 || op == OP_if_true8) {
            int tgt = pos + 1 + (int8_t)pc[pos + 1];

            targets[tgt] = 1;
            if (tgt < pos)              /* backward jump: a loop back edge */
                loop_back[tgt] = pos;
        } else if (op == OP_goto || op == OP_if_false || op == OP_if_true) {
            int tgt = pos + 1 + (int32_t)whale_get_u32(pc + pos + 1);

            targets[tgt] = 1;
            if (tgt < pos)
                loop_back[tgt] = pos;
        } else if (op >= OP_get_arg0 && op < OP_get_arg0 + 4) {
            args_used[op - OP_get_arg0] = 1;
            if (whale_next_is_field_read(pc, len, pos, sz))
                args_obj[op - OP_get_arg0] = 1;
        } else if (op == OP_get_arg) {
            int a = (int)whale_get_u16(pc + pos + 1);
            if (a >= 0 && a < arg_count) {
                args_used[a] = 1;
                if (whale_next_is_field_read(pc, len, pos, sz))
                    args_obj[a] = 1;
            }
        } else if (op == OP_fclosure || op == OP_fclosure8) {
            /* 'fclosure8 M' carries its pool index; the 5-byte 'fclosure'
               form follows a 'push_const M'. Either way this is a function
               declared in this body, and it lands in the local the next store
               writes - a compile-time fact, so a later call through that
               local can be expanded in place. */
            int m = (op == OP_fclosure8)
                        ? (int)pc[pos + 1]
                        : ((pos >= 5 && pc[pos - 5] == OP_push_const)
                               ? (int)whale_get_u32(pc + pos - 5 + 1) : -1);
            int npos = pos + sz;
            int ln = (npos < len) ? whale_store_local(pc, len, npos, whale_op_size_of(pc[npos])) : -1;

            if (m >= 0 && ln >= 0 && ln < var_count)
                whale_loc_fn[ln] = m;
        } else if (op == OP_object) {
            /* the next store into a local names the one holding this object */
            whale_obj_pending = 1;
        } else if (op == OP_inc_loc || op == OP_dec_loc) {
            {   /* these write a local too, so it is not a constant.
                   Recorded separately rather than folded into assign_cnt: an
                   induction variable is the best thing to pin, but only one of
                   them may be pinned per function. minmax has two (i and j) and
                   pinning both hangs it, while pinning either one alone is fine. */
                int a = (int)pc[pos + 1];
                if (a < var_count) {
                    assign_cnt[a] += 4;
                    induction[a] = 1;
                }
            }
        } else if (op >= OP_put_loc0 && op <= OP_put_loc3) {
            int a = op - OP_put_loc0;

            assign_cnt[a]++;
            if (whale_obj_pending && a < var_count) {
                whale_loc_obj[a] = 1;
                whale_obj_pending = 0;
            }
        } else if (op == OP_put_loc8) {
            if ((int)pc[pos + 1] < var_count) {
                assign_cnt[pc[pos + 1]]++;
                if (whale_obj_pending) {
                    whale_loc_obj[pc[pos + 1]] = 1;
                    whale_obj_pending = 0;
                }
            }
        } else if (op == OP_put_loc) {
            int a = (int)whale_get_u16(pc + pos + 1);

            if (a < var_count) {
                assign_cnt[a]++;
                if (whale_obj_pending) {
                    whale_loc_obj[a] = 1;
                    whale_obj_pending = 0;
                }
            }
        } else if (op == OP_get_field || op == OP_get_field2 || op == OP_put_field) {
            /* the offset the profile table is keyed by: the access site's operand,
               which is the opcode offset plus one */
            if (whale_field_sites_n < WHALE_ATOM_MAX)
                whale_field_sites[whale_field_sites_n++] = pos + 1;
        } else if (op == OP_define_field) {
            int atom = (int)whale_get_u32(pc + pos + 1);

            if (whale_def_atoms_n < WHALE_ATOM_MAX)
                whale_def_atoms[whale_def_atoms_n++] = atom;
        } else if (op == OP_put_field) {
            int atom = (int)whale_get_u32(pc + pos + 1);

            if (whale_put_atoms_n < WHALE_ATOM_MAX)
                whale_put_atoms[whale_put_atoms_n++] = atom;
        } else if (op == OP_call || (op >= OP_call0 && op <= OP_call3)) {
            whale_has_call = 1;
        } else if (op >= OP_get_var_ref0 && op < OP_get_var_ref0 + 4) {
            /* Only a slot used as a call target has to be this very function; an
               enclosing closure's variable is read as a number instead. */
            if (whale_assume_self && whale_var_ref_feeds_call(pc, len, pos, sz)) {
                var_ref_used[op - OP_get_var_ref0] = 1;
                whale_gen_self_ref = 1;
            }
        } else if (op == OP_get_var_ref) {
            int a = (int)whale_get_u16(pc + pos + 1);

            if (a >= 0 && a < 4 && whale_assume_self &&
                whale_var_ref_feeds_call(pc, len, pos, sz)) {
                var_ref_used[a] = 1;
                whale_gen_self_ref = 1;
            }
        }
        pos += sz;
    }

    {
        /* Count the back edges. A nested loop needs more live registers than a
           flat one, and pinning its counters is what made minmax spin for ever:
           with both induction variables pinned TinyCC has nowhere left to put a
           temporary. */
        int k, nloops = 0;

        /* loop_back[] is -1 for "no back edge", so a truth test counted every
           entry: arith has one loop and reported 21, which made the
           `whale_nloops <= 1` gate false for every function long enough to have
           a loop. The pins were effectively off. */
        for (k = 0; k < len; k++)
            if (loop_back[k] >= 0)
                nloops++;
        whale_nloops = nloops;
    }

    /* A store to a name no literal defined would add a property, which changes
       the shape and invalidates every cached value pointer. Claim the shape is
       fixed only when that cannot happen, and only in a function with no calls -
       an object that cannot be passed anywhere cannot be reshaped from outside
       either. */
    /* An object's field pointers stay valid as long as nothing adds or removes a
       property. A call could hand the object to something that does, so a call
       is only acceptable outside every loop: the common front-end shape is a
       factory called once before the loop (`var o = make(); for (...) ...`), and
       refusing that sent objfields to the interpreter at 26x. */
    whale_shape_stable = 1;
    {
        int k;

        for (k = 0; k < whale_call_n && whale_shape_stable; k++) {
            int tgt;

            for (tgt = 0; tgt < len; tgt++) {
                if (loop_back[tgt] >= 0 && whale_call_pos[k] >= tgt &&
                    whale_call_pos[k] <= loop_back[tgt]) {
                    whale_shape_stable = 0;
                    break;
                }
            }
        }
    }
    {
        int k;

        for (k = 0; k < whale_put_atoms_n && whale_shape_stable; k++) {
            int j, found = 0;

            for (j = 0; j < whale_def_atoms_n; j++)
                if (whale_def_atoms[j] == whale_put_atoms[k])
                    found = 1;
            if (!found)
                whale_shape_stable = 0;
        }
    }

    /* pass 2: emit tokens. Nothing here builds text - the generator pushes
       tokens into TinyCC's parser directly, so there is no lexer and no
       keyword table, and keywords are compile-time constants. */

    /* preamble: types only. JSValue must match quickjs.h exactly, because it is
       passed *by value* across the JIT/host boundary (function arguments and
       return values). quickjs.h uses the {union, int64_t tag} form on 64-bit,
       i.e. 16 bytes - NOT the 8-byte NAN-boxed uint64_t. Getting this wrong
       makes every JSValue argument/return garbage on the host side.

       From here the encoders write into the folding workspace; the preamble and
       the declarations below are structural, so they are committed as one
       block before the bytecode loop starts. */
    fold_begin();
    wk(JS_WK_TYPEDEF); wk(JS_WK_UNION); wt('{');
        wk(JS_WK_INT); wt_id("int32"); wt(';');
        wk(JS_WK_DOUBLE); wt_id("float64"); wt(';');
        wk(JS_WK_VOID); wt('*'); wt_id("ptr"); wt(';');
    wt('}'); wt_id("JSValueUnion"); wt(';');
    wk(JS_WK_TYPEDEF); wk(JS_WK_STRUCT); wt('{');
        wt_id("JSValueUnion"); wt_id("u"); wt(';');
        wk(JS_WK_LONG); wk(JS_WK_LONG); wt_id("tag"); wt(';');
    wt('}'); wt_id("JSValue"); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefGetNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(',');
        wk(JS_WK_INT); wt_id("idx"); wt(','); wk(JS_WK_DOUBLE); wt('*'); wt_id("out"); wt(')'); wt(';');    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefGetObj"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(',');
        wk(JS_WK_INT); wt_id("idx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefSetNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(',');
        wk(JS_WK_INT); wt_id("idx"); wt(','); wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefIsSelf"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt('*');
        wt_id("var_refs"); wt(','); wk(JS_WK_INT); wt_id("idx"); wt(',');
        wt_id("JSValue"); wt_id("self"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("whale_jit_box_num"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_DOUBLE); wt_id("d"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleCheckStack"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("whale_jit_unbox_num"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt_id("v"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("dst"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_NewFloat64"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_DOUBLE); wt_id("d"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_NewBool"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("b"); wt(')'); wt(';');
    /* typed-array helpers (WHALE_T_OBJ). Declared like everything else: with no
       prototype TinyCC applies the C89 implicit declaration, a `double` argument
       then goes to the wrong register, and every call silently turns into a
       deopt. */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleGetGlobalObj"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("atom"); wt(',');
        wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleNewU8Array"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("ctor_slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("n"); wt(','); wk(JS_WK_INT); wt_id("out_slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleTAU8Get"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("idx"); wt(','); wk(JS_WK_DOUBLE); wt('*'); wt_id("out"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleTAU8Set"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("idx"); wt(','); wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt_id("JS_WhaleTAU8Ptr"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("len_out"); wt(')'); wt(';');
    /* Plain-object fields. Every one of these needs a prototype here as well as
       a tcc_add_symbol: without it TinyCC falls back to an implicit declaration
       and the call passes the arguments wrong, which looks like the code simply
       not working. */
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt_id("JS_WhaleObjSlotNull"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("k"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjSlotAtom"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("atom"); wt(',');
        wk(JS_WK_INT); wt_id("k"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjDefineValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("atom"); wt(','); wk(JS_WK_INT); wt_id("val_slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjStoreValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("atom"); wt(','); wk(JS_WK_INT); wt_id("val_slot"); wt(',');
        wk(JS_WK_VOID); wt('*'); wt('*'); wt_id("shape_cache"); wt(',');
        wk(JS_WK_VOID); wt('*'); wt('*'); wt_id("ptr_cache"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleCpoolSlot"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("idx"); wt(',');
        wk(JS_WK_INT); wt_id("k"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleBinopVal"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("op"); wt(',');
        wk(JS_WK_INT); wt_id("a_slot"); wt(','); wk(JS_WK_INT); wt_id("b_slot"); wt(',');
        wk(JS_WK_INT); wt_id("out_slot"); wt(')'); wt(';');
    /* the single-instruction fallback (see the block comment above
       whale_step_emit): both a prototype here and a tcc_add_symbol, or TinyCC
       declares it implicitly and passes the arguments wrong */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleStepPushNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("i"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleStepPushSlot"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("i"); wt(',');
        wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleStepRun"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("pc_off"); wt(',');
        wk(JS_WK_INT); wt_id("n_pop"); wt(','); wk(JS_WK_INT); wt_id("n_push"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleStepOutIsNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("i"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_DOUBLE); wt_id("JS_WhaleStepOutNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("i"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleStepOutSlot"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("i"); wt(',');
        wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt_id("JS_WhaleStepClear"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("n"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjNew"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjDefineNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("atom"); wt(','); wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt('*'); wt_id("JS_WhaleObjShape"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt('*'); wt_id("JS_WhaleObjFieldPtr"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("atom"); wt(','); wk(JS_WK_VOID); wt('*'); wt('*'); wt_id("shape_io"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_DOUBLE); wt_id("JS_WhaleObjValueNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt('*'); wt_id("v"); wt(',');
        wk(JS_WK_INT); wt('*'); wt_id("deopt"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_WhaleObjGet"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("k"); wt(')'); wt(';');
    /* Packed arrays. Same warning as the object ones: a missing prototype here
       makes TinyCC declare the function implicitly, and then a double argument
       is passed as an int - the call still links and silently computes
       nonsense. */
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt('*'); wt_id("JS_WhaleArrPtr"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("len_out"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleArrNew"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleArrSetNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("idx"); wt(','); wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    /* Host calls. Declared like everything else: without a prototype TinyCC
       applies the C89 implicit declaration and a double argument goes to the
       wrong register - the call still links and silently computes nonsense. */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjSlotNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleArgGetObj"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt_id("v"); wt(',');
        wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjSlotCopy"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("src"); wt(',');
        wk(JS_WK_INT); wt_id("dst"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleCallValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("func_slot"); wt(',');
        wk(JS_WK_INT); wt_id("argc"); wt(','); wk(JS_WK_INT); wt_id("arg_base"); wt(',');
        wk(JS_WK_INT); wt_id("ret_slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_WhaleExceptionValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(')'); wt(';');
    /* object-slot test hook (WHALE_OBJ_TEST). Declared like the rest: without a
       prototype TinyCC applies the C89 implicit declaration, and while the int
       return happens to land correctly in eax here, relying on that is the same
       mistake that made every `%` return 0 (see the fmod note below). */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjProbeFrom"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt_id("v"); wt(')'); wt(';');
    /* `%` lowers to fmod(). It has to be declared: without this TinyCC falls
       back to the C89 implicit declaration, takes the int return value from rax
       while the real result is in xmm0, and every `%` silently yields 0. */
    wk(JS_WK_EXTERN); wk(JS_WK_DOUBLE); wt_id("fmod"); wt('(');
        wk(JS_WK_DOUBLE); wt_id("x"); wt(','); wk(JS_WK_DOUBLE); wt_id("y"); wt(')'); wt(';');
    /* the bitwise operators need JS ToInt32 semantics, not a C cast */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleToInt32"); wt('(');
        wk(JS_WK_DOUBLE); wt_id("d"); wt(')'); wt(';');
    /* reading a global (`get_var`); returns 0 when it is not a number */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleGetGlobalNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("atom"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("out"); wt(')'); wt(';');

    /* whale_jit_core_<n> works purely in doubles: every number is unboxed at the
       boundary. Self-recursion calls it directly, so the recursive fast path
       costs no boxing at all - only the engine entry does that. */
    wk(JS_WK_STATIC); wk(JS_WK_DOUBLE); wt_id(whale_core_name); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(',');
        wt_id("JSValue"); wt('*'); wt_id("args"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("An"); wt(',');
        wk(JS_WK_INT); wt('*'); wt_id("deopt"); wt(',');
        /* An array or object result cannot come back through the double return,
           so core writes the reference here; the numeric path leaves it
           undefined. */
        wt_id("JSValue"); wt('*'); wt_id("ret_obj"); wt(',');
        /* the closure slots, for reading a captured variable as a number */
        wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(')'); wt('{');
    /* Not initialised: `_cref` is only ever the out parameter of a self-recursive
       call, and those return numbers, so nothing reads it. Every statement here
       is one TinyCC spills, on every call. */
    wt_id("JSValue"); wt_id("_cref"); wt(';');
    /* Pin the hottest loop counter, and the hottest local that is not one.
       One counter only: minmax has two nested loops, each with its own counter,
       and pinning both makes it never terminate. Pinning either one alone is
       fine, and every case that measured 25-30% faster has a single loop.
       The second slot goes to the hottest non-counter - the accumulator shape.
       Two rather than more because the allocator has about fourteen float
       registers and a third pin already hangs on get_reg() exhaustion. */
    {
        int pin1 = -1, pin2 = -1, k;

        /* DISABLED: pinning is correct now but not profitable. Measured with
           temp/measure.ps1 (minimum of several runs): nested +11%,
           immutable_update +9%, sieve +6%, loop_sum +5%, against minmax -4%
           and array -5%. Two structural costs: every xmm is caller-saved so a
           pinned value is spilled at every call and reloaded after, and each
           pinned register is one less for TinyCC's own expression evaluation.
           Kept because the mechanism and its fixes are real (see cb87bf1) and
           because an ABI with non-volatile float registers would make it pay.
           See notes/pinning-design.md. */
        if (1) {
            for (k = 0; k < var_count; k++)
                if (induction[k] && (pin1 < 0 || assign_cnt[k] > assign_cnt[pin1]))
                    pin1 = k;
            for (k = 0; k < var_count; k++) {
                if (k == pin1 || induction[k] || assign_cnt[k] == 0)
                    continue;
                if (pin2 < 0 || assign_cnt[k] > assign_cnt[pin2])
                    pin2 = k;
            }
        }
        if (getenv("WHALE_PIN_DBG"))
            fprintf(stderr, "[pin] var_count=%d pin1=L%d pin2=L%d\n",
                    var_count, pin1, pin2);
        for (i = 0; i < var_count; i++) {
            if (i == pin1 || i == pin2)
                wk(JS_WK_REGISTER);
            wk(JS_WK_DOUBLE); wt_slot("L", i); wt('='); wt_int(0); wt(';');
        }
    }
    /* All of the generator's scratch state in one zero-initialised block: the
       value slots (stack_size plus the spare names fold_materialize() hands out,
       see whale_vname) and the per-slot caches. Declaring these as individual
       variables cost one statement each, and TinyCC spills at every statement -
       a short function spent nearly all of its time in its own prologue. */
    wk(JS_WK_STRUCT); wt('{');
        wk(JS_WK_DOUBLE); wt_id("v"); wt('['); wt_int(stack_size + WHALE_VSER_MAX); wt(']'); wt(';');
        wk(JS_WK_VOID); wt('*'); wt_id("fs"); wt('['); wt_int(WHALE_FIELD_CACHE); wt(']'); wt(';');
        wt_id("JSValue"); wt('*'); wt_id("fp"); wt('['); wt_int(WHALE_FIELD_CACHE); wt(']'); wt(';');
        wk(JS_WK_INT); wt_id("fver"); wt(';');
        wk(JS_WK_INT); wt_id("fv"); wt('['); wt_int(WHALE_FIELD_CACHE); wt(']'); wt(';');
        wt_id("JSValue"); wt('*'); wt_id("ap"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
        wk(JS_WK_DOUBLE); wt_id("an"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
        wk(JS_WK_INT); wt_id("ai"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
        wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt_id("op"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
        wk(JS_WK_DOUBLE); wt_id("on"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
        wk(JS_WK_INT); wt_id("oi"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
        /* unboxed struct fields, and a presence flag each: a field that only some of
           the observed shapes carry still gets its slot here */
        wk(JS_WK_DOUBLE); wt_id("sv"); wt('['); wt_int(WHALE_STRUCT_SLOTS * WHALE_STRUCT_FIELDS); wt(']'); wt(';');
        wk(JS_WK_UNSIGNED); wt_id("char"); wt_id("sp"); wt('['); wt_int(WHALE_STRUCT_SLOTS * WHALE_STRUCT_FIELDS); wt(']'); wt(';');
    wt('}'); wt_id("_ws"); wt('='); wt('{'); wt_int(0); wt('}'); wt(';');
    /* 1, because a cached fv[] of 0 (from that zero initialiser) must not look
       like the current generation. */
    wt_id("_ws"); wt('.'); wt_id("fver"); wt('='); wt_int(1); wt(';');
    /* Per-property-access cache for object literals: the shape the property was
       resolved against and the address of its value. One pair per access site,
       and a site always touches the same property, so this is a compile-time
       constant. */

    /* per-object-slot typed-array cache: base pointer, element count, and a flag
       so the pointer is fetched once per run rather than per element */
    /* op/on/oi are covered by the state block above */
    for (i = 0; i < arg_count; i++) {
        if (args_used[i]) {
            wk(JS_WK_DOUBLE); wt_slot("A", i); wt('='); wt_id("An"); wt('['); wt_int(i); wt(']'); wt(';');
        }
    }
    fold_commit(0);     /* the whole implicit prologue is structural */

    for (i = 0; i < var_count; i++)
        loc_slot[i] = -1;

    /* Slots for the arguments that travel as objects. Handed out before the body
       is generated so the entry unboxing and every `get_arg` agree on the number,
       and (like every other slot) numbered below the call scratch area. */
    for (i = 0; i < arg_count; i++) {
        arg_obj_slot[i] = -1;
        if (args_obj[i]) {
            if (whale_objs >= WHALE_CALL_BASE) {
                whale_objs = WHALE_CALL_BASE;   /* no room: refuse the function */
                goto out;
            }
            arg_obj_slot[i] = whale_objs++;
        }
    }

    sp = 0;
    for (pos = 0; pos < len; ) {
        int op = pc[pos];
        int sz = whale_op_size_of(op);
        int ta, tb, t;
        int k1 = 0, i1 = 0, k2 = 0, i2 = 0;
        int l1 = 0, l2 = 0, ok1, ok2 = 0, p2 = 0;
        int t1 = 0, t2 = 0;
        int c1 = 0, c2 = 0;
        double cv1 = 0, cv2 = 0;

        /* A loop head opens a buffer for its body; the back edge closes it.
           The head label goes inside the buffer, because that is where the back
           edge jumps to. Nesting uses the loop stack. */
        if (loop_back[pos] >= 0)
            loop_begin(pos, loop_back[pos]);

        /* dead code after an unconditional return/goto is not emitted */
        if (unreachable && !targets[pos]) {
            pos += sz;
            continue;
        }
        if (targets[pos])
            unreachable = 0;

        /* A basic-block boundary: the value of every deferred local has to be in
           its variable again, because the code that reaches this point may not
           be the code that set the pending expression. */
        if (targets[pos])
            local_flush_all(var_count);

        cur[state_len - 1] = (uint8_t)sp;
        if (targets[pos]) {
            if (seen[pos]) {
                size_t base = (size_t)pos * state_len;
                int snap_sp = snap[base + state_len - 1];
                int live = var_count + ((snap_sp < sp) ? snap_sp : sp);
                int k, differ = 0;

                for (k = 0; k < live; k++) {
                    uint8_t sv = snap[base + k];
                    uint8_t cv = cur[k];

                    if (sv == cv)
                        continue;
                    /* WHALE_T_I32 is an optimisation hint, not a value: an I32 on
                       one edge and a plain number on the other are the same thing
                       here, so merge them down to the plain number. Treating the
                       difference as a conflict refused to compile the whole
                       function - which is how `acc` (NUM down one branch of an
                       `if`, I32 down the other) lost bitops to the interpreter. */
                    if (sv == WHALE_T_I32 || sv == WHALE_T_ICONST) {
                        if (whale_type_is_num(cv)) {
                            snap[base + k] = WHALE_T_NUM;
                            continue;
                        }
                    }
                    if (cv == WHALE_T_I32 || cv == WHALE_T_ICONST) {
                        if (whale_type_is_num(sv)) {
                            snap[base + k] = WHALE_T_NUM;
                            continue;
                        }
                    }
                    if (sv == WHALE_T_UNKNOWN) {
                        snap[base + k] = cv; /* optimistic merge */
                    } else if (cv != WHALE_T_UNKNOWN) {
                        differ = 1;
                        break;
                    }
                }
                if (differ)
                    goto out;
            } else {
                seen[pos] = 1;
                memcpy(snap + (size_t)pos * state_len, cur, state_len);
            }
            /* A jump target's state comes from the edges that reach it, not from the
               linear scan: restoring the snapshot is what puts `sp` back after dead
               code was skipped (e.g. an else-branch sitting right after the goto). */
            memcpy(cur, snap + (size_t)pos * state_len, state_len);
            sp = cur[state_len - 1];
            /* A basic-block boundary: every live stack slot has to become a real
               variable again, because the expressions that produced them belong
               to the block that just ended. */
            {
                int k;

                for (k = 0; k < sp; k++)
                    fold_materialize(k);
            }
            {
                int fm = fold_mark();

                if (getenv("WHALE_STEP_DBG"))
                    fprintf(stderr, "[label] pos=%d\n", pos);
                wt_slot("L", pos); wt(':');
                fold_commit(fm);
            }
        }

        /* --- peephole 1: <load> <load> <binop> [put_loc] -> one assignment --- */
        ok1 = whale_load_analyze(info, pc, pos, cur, var_count, lo_c, lo_cv, &l1, &t1, &k1, &i1, &cv1);
        if (ok1) {
            p2 = pos + l1;
            if (p2 < len)
                ok2 = whale_load_analyze(info, pc, p2, cur, var_count, lo_c, lo_cv, &l2, &t2, &k2, &i2, &cv2);
        } else if (op == OP_get_loc0_loc1 && var_count >= 2 &&
                   cur[0] != WHALE_T_UNKNOWN && cur[1] != WHALE_T_UNKNOWN) {
            k1 = WLD_SLOT; i1 = 0;
            k2 = WLD_SLOT; i2 = 1;
            t1 = cur[0];
            t2 = cur[1];
            l1 = 1;
            l2 = 0;
            p2 = pos + 1;
            ok1 = ok2 = 1;
        }
        if (ok1 && ok2) {
            int p3 = p2 + l2;
            int op3 = (p3 < len) ? pc[p3] : 0;

            {
            int is_cmp = 0;
            int opid = whale_binop_op(op3, &is_cmp);

            if (opid >= 0 && whale_type_is_num(t1) && whale_type_is_num(t2)) {
                int l3 = whale_op_size_of(op3);
                int p4 = p3 + l3;
                int dst = -1, l4 = 0;

                if (p4 < len && pc[p4] >= OP_put_loc0 && pc[p4] <= OP_put_loc3) {
                    dst = pc[p4] - OP_put_loc0;
                    l4 = 1;
                } else if (p4 < len && pc[p4] == OP_put_loc8) {
                    dst = (int)pc[p4 + 1];
                    l4 = 2;
                } else if (p4 < len && pc[p4] == OP_put_loc) {
                    dst = (int)whale_get_u16(pc + p4 + 1);
                    l4 = 3;
                }

                /* Algebraic identities that remove the operation entirely, and
                   with it a whole statement: x*1, 1*x, x/1 and x-0 are all just
                   x. Deliberately NOT here: x+0 and 0+x (-0 + 0 is +0, so the
                   sign changes) and x*0 (NaN * 0 is NaN, not 0). */
                {
                    int id_keep = -1;    /* 0 = keep the left operand, 1 = right */

                    if (op3 == OP_mul) {
                        if ((k2 == WLD_INT || k2 == WLD_REAL) && cv2 == 1.0)
                            id_keep = 0;
                        else if ((k1 == WLD_INT || k1 == WLD_REAL) && cv1 == 1.0)
                            id_keep = 1;
                    } else if (op3 == OP_div) {
                        if ((k2 == WLD_INT || k2 == WLD_REAL) && cv2 == 1.0)
                            id_keep = 0;
                    } else if (op3 == OP_sub) {
                        /* x-0 keeps x, including -0 - 0 == -0 */
                        if ((k2 == WLD_INT || k2 == WLD_REAL) && cv2 == 0.0)
                            id_keep = 0;
                    }
                    if (id_keep >= 0) {
                        int fm = fold_mark();

                        if (dst >= 0) {
                            /* the variable is written here, so any pending
                               expression for it must be dropped first */
                            local_forget(dst);
                            wt_slot("L", dst); wt('=');
                            if (id_keep == 0) whale_load_emit(k1, i1, cv1);
                            else whale_load_emit(k2, i2, cv2);
                            wt(';');
                            fold_commit(fm);
                            cur[dst] = (uint8_t)(id_keep == 0 ? t1 : t2);
                        } else {
                            if (id_keep == 0) whale_load_emit(k1, i1, cv1);
                            else whale_load_emit(k2, i2, cv2);
                            fold_set(sp, fm);
                            cur[var_count + sp] = (uint8_t)(id_keep == 0 ? t1 : t2);
                            sp += 1;
                        }
                        pos = p4 + l4;
                        continue;
                    }
                }

                if (is_cmp) {
                    if (dst >= 0) {
                        int fm = fold_mark();

                        local_forget(dst);
                        wt_slot("L", dst); wt('='); wt('('); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0); wt(';');
                        fold_commit(fm);
                        cur[dst] = WHALE_T_BOOL;
                    } else {
                        int fm = fold_mark();

                        wt('('); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0);
                        fold_set(sp, fm);
                        cur[var_count + sp] = WHALE_T_BOOL;
                        sp += 1;
                    }
                } else {
                    if (dst >= 0) {
                        int fm = fold_mark();

                        local_forget(dst);
                        wt_slot("L", dst); wt('='); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(';');
                        fold_commit(fm);
                        cur[dst] = WHALE_T_NUM;
                    } else {
                        int fm = fold_mark();

                        whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2);
                        fold_set(sp, fm);
                        cur[var_count + sp] = WHALE_T_NUM;
                        sp += 1;
                    }
                }
                pos = p4 + l4;
                continue;
            }
            }
        }

        /* --- peephole 2: <load> put_loc -> direct copy --- */
        if (ok1 && whale_type_is_num(t1)) {
            int pb = pos + l1;
            int d2 = -1, l5 = 0;

            if (pb < len && pc[pb] >= OP_put_loc0 && pc[pb] <= OP_put_loc3) {
                d2 = pc[pb] - OP_put_loc0;
                l5 = 1;
            } else if (pb < len && pc[pb] == OP_put_loc8) {
                d2 = (int)pc[pb + 1];
                l5 = 2;
            } else if (pb < len && pc[pb] == OP_put_loc) {
                d2 = (int)whale_get_u16(pc + pb + 1);
                l5 = 3;
            }
            if (d2 >= 0 && d2 < var_count) {
                /* `L = L` is a leftover worth dropping, not emitting: the
                   QuickJS parser already folds things like `x * 1` into a
                   plain copy, and a store that changes nothing is pure
                   overhead in a loop body. Type bookkeeping below still runs. */
                if (!(k1 == WLD_SLOT && i1 == d2)) {
                    int fm = fold_mark();

                    local_forget(d2);
                    wt_slot("L", d2); wt('='); whale_load_emit(k1, i1, cv1); wt(';');
                    fold_commit(fm);
                }
                cur[d2] = (uint8_t)t1;
                if (assign_cnt[d2] == 1 && c1) {
                    lo_c[d2] = 1;
                    lo_cv[d2] = cv1;
                } else {
                    lo_c[d2] = 0;
                }
                pos = pb + l5;
                continue;
            }
        }

        /* Control flow is about to leave this position, and a pending local
           expression is not yet stored anywhere at run time: it would only be
           written where the value is next read, which the jump skips over. So
           every deferred local has to be in its variable before the jump. This
           is what a loop back edge needs - without it the loop counter is never
           updated and the loop does not terminate. */
        if (op == OP_goto || op == OP_goto8 || op == OP_goto16 ||
            op == OP_if_false || op == OP_if_false8 ||
            op == OP_if_true || op == OP_if_true8)
            local_flush_all(var_count);

        switch (op) {
        case OP_push_minus1:
        case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
        case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
            {
                int fm = fold_mark();

                wt_int(op == OP_push_minus1 ? -1 : op - OP_push_0);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;
        case OP_push_i8:
            {
                int fm = fold_mark();

                wt_int((int)(int8_t)pc[pos + 1]);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;
        case OP_push_i16:
            {
                int fm = fold_mark();

                wt_int((int)(int16_t)whale_get_u16(pc + pos + 1));
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;
        case OP_push_i32:
            {
                int fm = fold_mark();

                wt_int((int)(int32_t)whale_get_u32(pc + pos + 1));
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;
        case OP_fclosure:
        case OP_fclosure8:
            /* A function declaration. The pre-scan recorded which pool entry the
               local ends up holding, and that is what a call will use, so there
               is nothing to emit - pushing the closure as a value would put a
               function where the type system expects a number. The store that
               follows is part of the declaration and goes with it. */
            {
                int npos = pos + sz;
                int s4 = (npos < len) ? whale_op_size_of(pc[npos]) : 0;
                int ln = (s4 > 0) ? whale_store_local(pc, len, npos, s4) : -1;

                if (ln >= 0)
                    pos = npos + s4;
            }
            break;

        case OP_push_const:
        case OP_push_const8:
            {
                int idx = (op == OP_push_const8) ? (int)pc[pos + 1] : (int)whale_get_u32(pc + pos + 1);
                JSValue c;
                int ct, fm;

                if (idx < 0 || idx >= info->cpool_count)
                    goto out;
                c = info->cpool[idx];
                ct = JS_VALUE_GET_TAG(c);
                if (ct != JS_TAG_INT && !JS_TAG_IS_FLOAT64(ct))
                    goto out; /* strings / bigints / objects: not our shape */
                fm = fold_mark();
                if (ct == JS_TAG_INT)
                    wt_int((int)JS_VALUE_GET_INT(c));
                else
                    wt_num(JS_VALUE_GET_FLOAT64(c));
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;
        case OP_push_true:
            {
                int fm = fold_mark();

                wt_int(1);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_BOOL;
                sp++;
            }
            break;
        case OP_push_false:
            {
                int fm = fold_mark();

                wt_int(0);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_BOOL;
                sp++;
            }
            break;

        /* Property-key conversion. The JIT only holds numbers, so a value here is
           already its own property key and this is the identity. Checked rather
           than assumed: a non-number would mean something else got this far and
           must deopt. */
        case OP_to_propkey:
        case OP_to_propkey2:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            break;

        /* Is the value undefined or null? Every value the JIT can hold is a
           number, a boolean, this function or an object - never undefined and
           never null - so the answer is always false. Nothing can reach here
           holding one: OP_undefined has no case and get_var_undef deopts when
           the global is missing. */
        case OP_is_undefined_or_null:
            {
                int fm = fold_mark();

                wt_int(0);
                fold_set(sp - 1, fm);
                cur[var_count + sp - 1] = WHALE_T_BOOL;
            }
            break;

        /* Exchange the top two values. Materialising both first reduces this to
           a swap of the two stack slots' bookkeeping - including the object slot
           number, which rides in the type byte, so it needs nothing extra. */
        case OP_swap:
            {
                int a = sp - 2, b = sp - 1;
                WhaleVExpr ve;
                uint8_t tb;
                int tn;

                fold_materialize(a);
                fold_materialize(b);
                ve = whale_vexpr[a]; whale_vexpr[a] = whale_vexpr[b]; whale_vexpr[b] = ve;
                tb = whale_vmat[a];  whale_vmat[a]  = whale_vmat[b];  whale_vmat[b]  = tb;
                tn = whale_vname[a]; whale_vname[a] = whale_vname[b]; whale_vname[b] = tn;
                tb = cur[var_count + a];
                cur[var_count + a] = cur[var_count + b];
                cur[var_count + b] = tb;
            }
            break;

        /* `new Uint8Array(n)`. Stack is [ctor, new.target, args...]; only the
           one-argument typed-array form is lowered, everything else deopts. */
        case OP_call_constructor:
            {
                int argc = (int)whale_get_u16(pc + pos + 1);
                int fslot = sp - argc - 2;
                int out;

                if (argc != 1 || fslot < 0)
                    goto out;
                /* new.target is deliberately not checked: for `new Uint8Array(n)`
                   the bytecode passes the constructor itself as the target, and a
                   target that is some other function cannot change what this
                   construction makes. */
                if (!WHALE_IS_OBJ(cur[var_count + fslot]) ||
                    !whale_type_is_num(cur[var_count + sp - 1]))
                    goto out;
                out = whale_objs++;
                if (out >= WHALE_OBJ_SLOTS)
                    goto out;
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!');
                    wt_id("JS_WhaleNewU8Array"); wt('('); wt_id("ctx"); wt(',');
                    wt_int(WHALE_OBJ_SLOT(cur[var_count + fslot])); wt(',');
                    fold_inline(sp - 1); wt(','); wt_int(out); wt(')');
                    wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                }
                sp = fslot;
                cur[var_count + sp] = WHALE_OBJ_T(out);
                sp++;
            }
            break;

        /* arr[idx] on a typed array. The array is a slot number, so only the
           index needs a value. */
        case OP_get_array_el:
            {
                int ta = cur[var_count + sp - 2];
                int tb = cur[var_count + sp - 1];
                int os, nm, tk;

                /* What makes either branch below refuse - an index that is not a
                   number, or a container this generator does not know - becomes
                   one instruction handed to the engine. */
                if ((!whale_type_is_num(tb) ||
                     (!WHALE_IS_ARR(ta) && !WHALE_IS_SLOT(ta))) &&
                    whale_step_emit(OP_get_array_el, pos, &sp, cur, var_count,
                                    stack_size))
                    break;
                /* A packed array: the elements are JSValue, so the value is read
                   inline exactly as an object field is. */
                if (WHALE_IS_ARR(ta)) {
                    if (!whale_type_is_num(tb))
                        goto out;
                    os = WHALE_OBJ_SLOT(ta);
                    nm = whale_vserial++;      /* result */
                    tk = whale_vserial++;      /* the index, evaluated once */
                    {
                        int fm = fold_mark();

                        /* resolve the backing store once; a scan does not move it */
                        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ai", os); wt(')'); wt('{');
                        wt_arr("ap", os); wt('='); wt_id("JS_WhaleArrPtr"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_arr("an", os); wt(')'); wt(';');
                        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ap", os); wt(')'); wt('{');
                        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        wt_arr("ai", os); wt('='); wt_int(1); wt(';');
                        wt('}');
                        /* the index, evaluated once and range-checked */
                        wt_arr("v", tk); wt('='); fold_inline(sp - 1); wt(';');
                        wk(JS_WK_IF); wt('('); wt_arr("v", tk); wt_op(JS_OP_LT); wt_int(0); wt_op(JS_OP_OR);
                        wt('('); wt_arr("v", tk); wt_op(JS_OP_GE); wt_arr("an", os); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        /* int32 or float64 inline; anything else hands off */
                        wt_arr("v", nm); wt('=');
                        wt('('); wt_arr("ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_arr("v", tk); wt(']');
                        /* `ap[os][i]` is a JSValue - a value, not a pointer - so
                           member access is `.`. Spelling it `->` is what TinyCC
                           reports as "pointer expected", and it stopped every
                           function that read an array element as a number from
                           compiling at all. */
                        wt_op(JS_OP_DOT); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
                        wt('?'); wt_arr("ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_arr("v", tk); wt(']');
                        wt_op(JS_OP_DOT); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
                        wt(':'); wt('('); wt_arr("ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_arr("v", tk); wt(']');
                        wt_op(JS_OP_DOT); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
                        wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_arr("ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_arr("v", tk); wt(']');
                        wt_op(JS_OP_DOT); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
                        wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(',');
                        wt('&'); wt_arr("ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_arr("v", tk); wt(']'); wt(','); wt_id("deopt"); wt(')');
                        wt(';');
                        fold_commit(fm);
                        whale_vname[sp - 2] = nm;
                        whale_vmat[sp - 2] = 1;
                    }
                    cur[var_count + sp - 2] = WHALE_T_NUM;
                    sp--;
                    break;
                }
                if (!WHALE_IS_SLOT(ta) || !whale_type_is_num(tb))
                    goto out;
                os = WHALE_SLOT(ta);
                nm = whale_vserial++;      /* result */
                tk = whale_vserial++;      /* the index, evaluated once */
                {
                    int fm = fold_mark();

                    /* Fetch the buffer once per run, then index it directly. A
                       call per element costs more than the interpreter. */
                    wk(JS_WK_IF); wt('('); wt('!'); wt_arr("oi", os); wt(')'); wt('{');
                    wt_arr("op", os); wt('='); wt('('); wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt(')');
                    wt_id("JS_WhaleTAU8Ptr"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_arr("on", os); wt(')'); wt(';');
                    wt_arr("oi", os); wt('='); wt_int(1); wt(';'); wt('}');
                    wk(JS_WK_DOUBLE); wt_slot("_t", tk); wt('='); fold_inline(sp - 1); wt(';');
                    wk(JS_WK_IF); wt('('); wt('('); wt('!'); wt_arr("op", os); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_LT); wt_int(0); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_GE); wt_arr("on", os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    wt_arr("v", nm); wt('='); wt('('); wk(JS_WK_DOUBLE); wt(')');
                    wt_arr("op", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("_t", tk); wt(']'); wt(';');
                    fold_commit(fm);
                    whale_vname[sp - 2] = nm;
                    whale_vmat[sp - 2] = 1;
                }
                cur[var_count + sp - 2] = WHALE_T_NUM;
                sp--;
            }
            break;

        /* --- object literals and their fields ---------------------------------
           Only a local object literal is handled, and only field access on it.
           A helper call per access would lose to the interpreter, so the
           helpers take a cache pair the generated code owns: they compare the
           object's shape and, when it is unchanged, go straight to the value.
           Anything unexpected (missing field, accessor, a value that is not a
           number) deopts. */
        case OP_object:
            {
                int os;
                int mono = 1, si;

                /* Was every field access site in this function observed holding a
                   single shape? If so, the objects built here have a settled layout
                   and can be laid out as a struct: one load per field instead of the
                   7-memory-operation sequence.

                   Asked at the function level because per-object would need a
                   provenance array kept in step with cur[] and snapshotted at every
                   jump target. Both cases with a measurement behind them build
                   exactly one object, so this covers them; a function with two
                   objects of different shapes is refused, which is the conservative
                   direction.

                   The reservation rule applies inside: the layout is the union of the
                   observed shapes, so a field only some of them carry still gets a
                   slot, because reserving eight bytes beats falling back. */
                for (si = 0; mono && si < whale_field_sites_n; si++)
                    if (!whale_prof_site_ok((unsigned)whale_field_sites[si]))
                        mono = 0;
                if (whale_field_sites_n == 0)
                    mono = 0;
                if (getenv("WHALE_PROF_DBG"))
                    fprintf(stderr, "[obj] pos=%d sites=%d mono=%d\n",
                            pos, whale_field_sites_n, mono);

                if (whale_objs >= WHALE_CALL_BASE)
                    goto out;
                os = whale_objs++;
                /* A settled layout: hold the object as a C struct in the frame
                   instead of a heap object with a shape, and let the four field
                   access ops read and write its columns directly - one access, no
                   shape check, no pointer chase, no tag branch.

                   On by default. An earlier revision had this off and recorded it
                   as "19x slower than node"; that measurement was wrong. The struct
                   was being created while the accesses still went through
                   JS_WhaleObjFieldPtr, and on top of that a struct-typed local was
                   still being materialised into a C variable - which emitted
                   `L0 = ( )` and made the function fail to compile entirely, so the
                   timing was the interpreter, not the struct. With both halves
                   right:

                       WHALE_NO_UNBOX=1   object 7ms   objshape 823ms
                       default            object 4ms   objshape 563ms
                       stock qjs          object 386ms objshape 1241ms */
                if (!getenv("WHALE_NO_UNBOX") && mono && whale_str_n == 0) {
                    int fa[WHALE_STRUCT_FIELDS];
                    int fn = 0, si2;

                    /* The layout is the union of the atoms the field accesses were
                       observed to use. The offset to ask about is the *access
                       site's* (whale_field_sites), not this OP_object's: the profile
                       is keyed by where the read or write happened, so passing the
                       literal's own offset always came back empty - which is why the
                       struct machinery looked dead. */
                    for (si2 = 0;
                         si2 < whale_field_sites_n && fn < WHALE_STRUCT_FIELDS; si2++) {
                        int one[WHALE_STRUCT_FIELDS];
                        int on = whale_struct_fields((unsigned)whale_field_sites[si2],
                                                     one, WHALE_STRUCT_FIELDS);
                        int q;

                        for (q = 0; q < on; q++) {
                            int seen = 0, m;

                            for (m = 0; m < fn; m++)
                                if (fa[m] == one[q]) { seen = 1; break; }
                            if (!seen)
                                fa[fn++] = one[q];
                        }
                    }

                    if (fn > 0) {
                        int k, pfm = fold_mark();

                        for (k = 0; k < fn; k++) {
                            whale_str_atoms[k] = fa[k];
                            /* sp is the presence flag: a field nobody assigned must
                               not read as a number. */
                            wt_arr("sp", k); wt('='); wt_int(0); wt(';');
                            wt_arr("sv", k); wt('='); wt_int(0); wt(';');
                        }
                        fold_commit(pfm);
                        whale_str_n = fn;
                        whale_str_slot = os;
                        cur[var_count + sp] = WHALE_STRUCT_T(os);
                        sp++;
                        break;
                    }
                }
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjNew"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                }
                cur[var_count + sp] = WHALE_OBJ_T(os);
                sp++;
            }
            break;

        /* The two instructions that push a value the generator cannot do arithmetic
           on. Neither is a failure: the value is parked in an object slot as an
           opaque JSValue, exactly as the call path already parks a live result, and
           the type carries WHALE_T_VAL so anything downstream that does need a
           number refuses instead of computing something wrong.

           Abandoning the whole function over these is what kept vnode_build out of
           the JIT entirely - a vnode record holds a string key and a null initial
           value, so both instructions appear before the first field store. */
        case OP_null:
            {
                int os;

                if (whale_objs >= WHALE_CALL_BASE)
                    goto out;
                os = whale_objs++;
                {
                    int fm = fold_mark();

                    wt_id("JS_WhaleObjSlotNull"); wt('('); wt_id("ctx"); wt(',');
                    wt_int(os); wt(')'); wt(';');
                    fold_commit(fm);
                }
                cur[var_count + sp] = WHALE_VAL_T(os);
                sp++;
            }
            break;

        case OP_push_atom_value:
            {
                int atom = (int)whale_get_u32(pc + pos + 1);
                int os;

                if (whale_objs >= WHALE_CALL_BASE)
                    goto out;
                os = whale_objs++;
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjSlotAtom"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(atom); wt(','); wt_int(os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                }
                cur[var_count + sp] = WHALE_VAL_T(os);
                sp++;
            }
            break;

        /* obj value -> obj */
        case OP_define_field:
            {
                int ta = cur[var_count + sp - 2];
                int tb = cur[var_count + sp - 1];
                int atom = (int)whale_get_u32(pc + pos + 1);

                /* The fields of an unboxed struct are frame variables, not
                   properties: the literal's initialisers become direct stores into
                   the layout's columns. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);

                    if (col < 0 || !whale_type_is_num(tb))
                        goto out;
                    {
                        int fm = fold_mark();

                        wt_arr("sv", col); wt('='); fold_inline(sp - 1); wt(';');
                        wt_arr("sp", col); wt('='); wt_int(1); wt(';');
                        fold_commit(fm);
                    }
                    sp--;   /* the object stays on the stack */
                    break;
                }
                if (!WHALE_IS_SLOT(ta))
                    goto out;
                if (!whale_type_is_num(tb)) {
                    /* { key: "a string" } - the value is opaque, so hand it to the
                       engine rather than abandon the function. This is the last
                       thing standing between vnode_build and compilation. */
                    if (!WHALE_IS_VAL(tb))
                        goto out;
                    {
                        int fm = fold_mark();

                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineValue"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(WHALE_SLOT(ta)); wt(','); wt_int(atom);
                        wt(','); wt_int(WHALE_VAL_SLOT(tb)); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    sp--;
                    break;
                }
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(WHALE_SLOT(ta)); wt(','); wt_int(atom); wt(',');
                    fold_inline(sp - 1); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                }
                sp--;   /* the object stays on the stack */
            }
            break;

        /* obj value -> nothing */
        case OP_put_field:
            {
                int ta = cur[var_count + sp - 2];
                int tb = cur[var_count + sp - 1];
                int atom = (int)whale_get_u32(pc + pos + 1);

                /* A struct field is a frame variable: the store is one
                   assignment - no shape check, no pointer chase, no tag branch.
                   That is the whole point of laying the object out. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);

                    if (col < 0 || !whale_type_is_num(tb))
                        goto out;
                    {
                        int fm = fold_mark();

                        wt_arr("sv", col); wt('='); fold_inline(sp - 1); wt(';');
                        wt_arr("sp", col); wt('='); wt_int(1); wt(';');
                        fold_commit(fm);
                    }
                    sp -= 2;
                    break;
                }
                if (getenv("WHALE_JIT_DUMP"))
                    fprintf(stderr, "[put] pos=%d ta=%d slot=%d tb=%d isnum=%d fields=%d isobj=%d\n",
                            pos, ta, WHALE_IS_SLOT(ta), tb, whale_type_is_num(tb),
                            whale_fields, WHALE_IS_OBJ(ta));
                /* A store this generator cannot lower - an operand that is
                   neither a number nor a parked value, or a shape it has no
                   cache left for - is handed to the engine as one instruction
                   instead of abandoning the function (see whale_step_emit). */
                if ((!WHALE_IS_SLOT(ta) || whale_fields >= WHALE_FIELD_CACHE ||
                     (!whale_type_is_num(tb) && !WHALE_IS_VAL(tb))) &&
                    whale_step_emit(OP_put_field, pos, &sp, cur, var_count,
                                    stack_size))
                    break;
                if (!WHALE_IS_SLOT(ta))
                    goto out;
                if (whale_fields >= WHALE_FIELD_CACHE)
                    goto out;
                if (!whale_type_is_num(tb) && !WHALE_IS_VAL(tb))
                    goto out;
                {
                    int fc = whale_fields++;
                    int fm;

                    /* `stable` means "the shape cannot change for the rest of
                       this function", which was proved about a local literal.
                       An object that came from a call is not one, so it always
                       keeps its shape check. */
                    fold_obj_field_resolve(WHALE_SLOT(ta), atom, fc,
                                           (whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1))) && WHALE_IS_OBJ(ta));
                    fm = fold_mark();
                    if (whale_type_is_num(tb)) {
                        /* the value is a double here, and the old one was a number
                           too (anything else deopted above) */
                        wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt('='); wt_int(8); wt(';');
                        wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt('=');
                        fold_inline(sp - 1); wt(';');
                    } else {
                        /* an opaque value - a string, a null, a value from a call.
                           Hand the store to the engine rather than abandon the
                           function; the cache pair is what the resolve above filled
                           in for the numeric path too. */
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjStoreValue"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(WHALE_SLOT(ta)); wt(','); wt_int(atom); wt(',');
                        wt_int(WHALE_VAL_SLOT(tb)); wt(',');
                        wt('&'); wt_arr("fs", fc); wt(','); wt('&'); wt_arr("fp", fc);
                        wt(')'); wt(')'); wt('{');
                        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    }
                    fold_commit(fm);
                }
                sp -= 2;
            }
            break;

        /* obj -> obj value. This is the shape `o.a += x` compiles to: the object
           stays on the stack under the value so the store can follow. */
        case OP_get_field2:
            {
                int ta = cur[var_count + sp - 1];
                int atom = (int)whale_get_u32(pc + pos + 1);
                int fc, nm;

                /* The receiver stays on the stack under the value - that is what
                   `get_field2` is for - so a struct receiver keeps its slot type
                   and the field value is pushed above it. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);

                    if (col < 0 || whale_vserial >= stack_size + WHALE_VSER_MAX)
                        goto out;
                    nm = whale_vserial++;
                    {
                        int fm = fold_mark();

                        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("sp", col); wt(')'); wt('{');
                        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        wt_arr("v", nm); wt('='); wt_arr("sv", col); wt(';');
                        fold_commit(fm);
                        whale_vname[sp] = nm;
                        whale_vmat[sp] = 1;
                    }
                    cur[var_count + sp] = WHALE_T_NUM;
                    sp++;
                    break;
                }
                if (!WHALE_IS_SLOT(ta))
                    goto out;
                if (whale_fields >= WHALE_FIELD_CACHE)
                    goto out;
                fc = whale_fields++;
                nm = whale_vserial++;
                fold_obj_field_load(WHALE_SLOT(ta), atom, fc, nm,
                                    (whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1))) && WHALE_IS_OBJ(ta));
                whale_vname[sp] = nm;
                whale_vmat[sp] = 1;
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;

        /* obj -> value */
        case OP_get_field:
            {
                int ta = cur[var_count + sp - 1];
                int atom = (int)whale_get_u32(pc + pos + 1);
                int fc, nm;

                /* A struct field is a frame variable: one load, with a presence
                   flag in place of a tag branch. The flag matters because a field
                   the layout reserves but this object never assigned must not read
                   as a number. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);

                    if (col < 0)
                        goto out;
                    nm = whale_vserial++;
                    {
                        int fm = fold_mark();

                        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("sp", col); wt(')'); wt('{');
                        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        wt_arr("v", nm); wt('='); wt_arr("sv", col); wt(';');
                        fold_commit(fm);
                        whale_vname[sp - 1] = nm;
                        whale_vmat[sp - 1] = 1;
                    }
                    cur[var_count + sp - 1] = WHALE_T_NUM;
                    break;
                }
                /* Refusals below (the receiver is not an object slot, or the
                   cache is full) become one instruction handed to the engine. */
                if ((!WHALE_IS_SLOT(ta) || whale_fields >= WHALE_FIELD_CACHE) &&
                    whale_step_emit(OP_get_field, pos, &sp, cur, var_count,
                                    stack_size))
                    break;
                if (!WHALE_IS_SLOT(ta))
                    goto out;
                if (whale_fields >= WHALE_FIELD_CACHE)
                    goto out;
                fc = whale_fields++;
                nm = whale_vserial++;
                fold_obj_field_load(WHALE_SLOT(ta), atom, fc, nm,
                                    (whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1))) && WHALE_IS_OBJ(ta));
                whale_vname[sp - 1] = nm;
                whale_vmat[sp - 1] = 1;
                cur[var_count + sp - 1] = WHALE_T_NUM;
            }
            break;

        /* --- packed arrays -----------------------------------------------------
           Front-end code is full of `a = []; for (i < n) a[i] = ...` and the
           matching scan. The elements are JSValue, so the value handling is the
           same as the object path's; only the structure needs the engine. */
        case OP_array_from:
            {
                int argc = (int)whale_get_u16(pc + pos + 1);
                int os;

                if (argc != 0)
                    goto out;           /* only the empty literal `[]` */
                if (whale_objs >= WHALE_CALL_BASE)
                    goto out;
                os = whale_objs++;
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleArrNew"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                }
                cur[var_count + sp] = WHALE_ARR_T(os);
                sp++;
            }
            break;

        /* obj -> length */
        case OP_get_length:
            {
                int ta = cur[var_count + sp - 1];
                int os, nm;

                /* A length that is not this generator's packed array: the
                   engine knows how to ask, so hand it the one instruction
                   rather than stopping compilation here. */
                if (!WHALE_IS_ARR(ta) &&
                    whale_step_emit(OP_get_length, pos, &sp, cur, var_count,
                                    stack_size))
                    break;
                if (!WHALE_IS_ARR(ta))
                    goto out;
                os = WHALE_OBJ_SLOT(ta);
                nm = whale_vserial++;
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ai", os); wt(')'); wt('{');
                    wt_arr("ap", os); wt('='); wt_id("JS_WhaleArrPtr"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_arr("an", os); wt(')'); wt(';');
                    wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ap", os); wt(')'); wt('{');
                    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    wt_arr("ai", os); wt('='); wt_int(1); wt(';');
                    wt('}');
                    wt_arr("v", nm); wt('='); wt_arr("an", os); wt(';');
                    fold_commit(fm);
                    whale_vname[sp - 1] = nm;
                    whale_vmat[sp - 1] = 1;
                }
                cur[var_count + sp - 1] = WHALE_T_NUM;
            }
            break;

        /* arr[idx] = v on a typed array, or on a packed array */
        case OP_put_array_el:
            {
                int ta = cur[var_count + sp - 3];
                int tb = cur[var_count + sp - 2];
                int tc = cur[var_count + sp - 1];
                int os, tk;

                /* What makes either branch below refuse - a non-numeric index or
                   value, or a container this generator does not know - becomes
                   one instruction handed to the engine. */
                if ((!whale_type_is_num(tb) || !whale_type_is_num(tc) ||
                     (!WHALE_IS_ARR(ta) && !WHALE_IS_SLOT(ta))) &&
                    whale_step_emit(OP_put_array_el, pos, &sp, cur, var_count,
                                    stack_size))
                    break;
                if (WHALE_IS_ARR(ta)) {
                    if (!whale_type_is_num(tb) || !whale_type_is_num(tc))
                        goto out;
                    os = WHALE_OBJ_SLOT(ta);
                    tk = whale_vserial++;      /* the index, evaluated once */
                    {
                        int fm = fold_mark();

                        /* A store can move the backing store (the array grows), so
                           the cached pointer must not be trusted afterwards.
                           Dropping the flag costs one re-resolve per store. */
                        wt_arr("ai", os); wt('='); wt_int(0); wt(';');
                        wt_arr("v", tk); wt('='); fold_inline(sp - 2); wt(';');
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleArrSetNum"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_arr("v", tk); wt(',');
                        fold_inline(sp - 1); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    sp -= 3;
                    break;
                }
                if (!WHALE_IS_SLOT(ta) || !whale_type_is_num(tb) ||
                    !whale_type_is_num(tc))
                    goto out;
                os = WHALE_SLOT(ta);
                tk = whale_vserial++;      /* the index, evaluated once */
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_arr("oi", os); wt(')'); wt('{');
                    wt_arr("op", os); wt('='); wt('('); wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt(')');
                    wt_id("JS_WhaleTAU8Ptr"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_arr("on", os); wt(')'); wt(';');
                    wt_arr("oi", os); wt('='); wt_int(1); wt(';'); wt('}');
                    wk(JS_WK_DOUBLE); wt_slot("_t", tk); wt('='); fold_inline(sp - 2); wt(';');
                    wk(JS_WK_IF); wt('('); wt('('); wt('!'); wt_arr("op", os); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_LT); wt_int(0); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_GE); wt_arr("on", os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    wt_arr("op", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("_t", tk); wt(']'); wt('=');
                    wt('('); wk(JS_WK_UNSIGNED); wt_id("char"); wt(')'); wt('('); fold_inline(sp - 1); wt(')'); wt(';');
                    fold_commit(fm);
                }
                sp -= 3;
            }
            break;

        case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
        case OP_get_loc8:
        case OP_get_loc:
            {
                int idx = (op == OP_get_loc8) ? (int)pc[pos + 1] :
                          (op == OP_get_loc) ? (int)whale_get_u16(pc + pos + 1) :
                          op - OP_get_loc0;
                t = (idx >= 0 && idx < var_count) ? cur[idx] : WHALE_T_UNKNOWN;
                if (t == WHALE_T_UNKNOWN)
                    goto out;
                if (WHALE_IS_SLOT(t) || WHALE_IS_STRUCT(t)) {
                    /* An object - or a laid-out struct - is its slot number, which
                       the type already carries. Nothing to emit: the value is the
                       same in every copy, and a struct has no C variable to load
                       from. Only the numeric path needs folding. */
                    cur[var_count + sp] = (uint8_t)t;
                    sp++;
                    break;
                }                if (whale_loc_fn[idx] >= 0) {
                    /* this local holds a function this compilation can see */
                    cur[var_count + sp] = WHALE_T_FN;
                    whale_slot_fn[sp] = whale_loc_fn[idx];
                    sp++;
                    break;
                }
                {
                    int fm = fold_mark();

                    local_inline(idx);
                    fold_set(sp, fm);
                }
                cur[var_count + sp] = (uint8_t)t;
                sp++;
            }
            break;
        case OP_put_loc0: case OP_put_loc1: case OP_put_loc2: case OP_put_loc3:
        case OP_put_loc8:
        case OP_put_loc:
            {
                int idx = (op == OP_put_loc8) ? (int)pc[pos + 1] :
                          (op == OP_put_loc) ? (int)whale_get_u16(pc + pos + 1) :
                          op - OP_put_loc0;
                int fm;

                t = cur[var_count + sp - 1];
                if (t == WHALE_T_UNKNOWN || idx < 0 || idx >= var_count)
                    goto out;
                if (getenv("WHALE_MEM_TRACE"))
                    fprintf(stderr, "[put_loc] L%d t=%d is_struct=%d is_slot=%d\n",
                            idx, t, WHALE_IS_STRUCT(t), WHALE_IS_SLOT(t));
                if (WHALE_IS_STRUCT(t)) {
                    /* A struct local carries the type and nothing else: the layout
                       columns (_ws.sv / _ws.sp) are the storage, so there is no C
                       variable and no object slot.

                       This has to be tested *before* WHALE_IS_SLOT, which
                       deliberately does not include structs. Placed inside that
                       branch it is unreachable - and then the deferred-store path
                       emits `L0 = ( )`, because a struct has no expression to
                       inline. */
                    local_forget(idx);
                    cur[idx] = (uint8_t)t;
                    lo_c[idx] = 0;
                    sp--;
                    break;
                }
                if (WHALE_IS_SLOT(t)) {
                    /* The value *is* a slot number, so this store is a type
                       change and nothing else - no deferred store and no fold
                       slot. It can put a different array in the slot, so drop
                       any cached buffer pointer.

                       A local that ever holds one of these gets one slot of its
                       own, because otherwise two assignments produce different
                       slot numbers, meet at the loop head as different types and
                       merge to UNKNOWN - which bails the whole function out. The
                       copy costs one dup per assignment; what it buys is that
                       the type of the variable never changes shape. */
                    int fm;
                    int src = WHALE_SLOT(t);

                    if (WHALE_IS_ARR(t)) {
                        /* A packed array keeps its own type rather than becoming
                           a plain slot value: the element access ops require it,
                           and unlike an object there is no second reading of the
                           same variable that could disagree with this one. */
                        fm = fold_mark();
                        wt_arr("oi", src); wt('='); wt_int(0); wt(';');
                        fold_commit(fm);
                        local_forget(idx);
                        cur[idx] = (uint8_t)t;
                        lo_c[idx] = 0;
                        sp--;
                        break;
                    }
                    if (WHALE_IS_STRUCT(t)) {
                        /* A struct is not a value at all - it is frame variables
                           (_ws.sv / _ws.sp), and the local only carries the type.
                           Nothing to emit, and nothing to unify either: there is
                           one layout per function, so every assignment names the
                           same slot. Emitting the local here is what produced
                           `L0 = ( ( ) )` - an empty expression, because a struct
                           has no C variable to read. */
                        local_forget(idx);
                        cur[idx] = (uint8_t)t;
                        lo_c[idx] = 0;
                        sp--;
                        break;
                    }
                    {
                        if (loc_slot[idx] < 0) {
                            if (whale_objs >= WHALE_CALL_BASE)
                                goto out;
                            loc_slot[idx] = whale_objs++;
                        }
                        if (src != loc_slot[idx]) {
                            fm = fold_mark();
                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjSlotCopy"); wt('(');
                            wt_id("ctx"); wt(','); wt_int(src); wt(','); wt_int(loc_slot[idx]); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                            fold_commit(fm);
                        }
                        t = WHALE_VAL_T(loc_slot[idx]);
                        src = loc_slot[idx];
                    }
                    fm = fold_mark();
                    wt_arr("oi", src); wt('='); wt_int(0); wt(';');
                    fold_commit(fm);
                    local_forget(idx);
                    cur[idx] = (uint8_t)t;
                    lo_c[idx] = 0;                    fm = fold_mark();
                    wt_id("_ws"); wt('.'); wt_id("fver"); wt('=');
                    wt_id("_ws"); wt('.'); wt_id("fver"); wt_op(JS_OP_ADD); wt_int(1); wt(';');
                    fold_commit(fm);
                    sp--;
                    break;
                }
                /* Defer the store: the value stays an expression until it is
                   read, or until the basic block ends. */
                local_forget(idx);
                fm = fold_mark();
                fold_inline(sp - 1);
                local_set(idx, fm);
                cur[idx] = (uint8_t)t;
                lo_c[idx] = 0;
                sp--;
            }
            break;
        /* set_loc* is put_loc with n_push=1: the value is stored *and* left on
           the stack, so `sp` deliberately does not move. */
        case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
        case OP_set_loc8:
        case OP_set_loc:
            {
                int idx = (op == OP_set_loc8) ? (int)pc[pos + 1] :
                          (op == OP_set_loc) ? (int)whale_get_u16(pc + pos + 1) :
                          op - OP_set_loc0;
                int fm;

                t = cur[var_count + sp - 1];
                if (t == WHALE_T_UNKNOWN || idx < 0 || idx >= var_count)
                    goto out;
                if (WHALE_IS_STRUCT(t)) {
                    /* See put_loc: a struct local carries the type and nothing
                       else - the layout columns are the storage. The value stays
                       on the stack, which is what set_loc is for. */
                    local_forget(idx);
                    cur[idx] = (uint8_t)t;
                    lo_c[idx] = 0;
                    break;
                }
                if (WHALE_IS_ARR(t)) {
                    /* An array keeps its own type - see the note in put_loc. */
                    local_forget(idx);
                    fm = fold_mark();
                    wt_arr("oi", WHALE_SLOT(t)); wt('='); wt_int(0); wt(';');
                    fold_commit(fm);
                    cur[idx] = (uint8_t)t;
                    lo_c[idx] = 0;
                    break;
                }
                if (WHALE_IS_SLOT(t)) {
                    /* The same slot unification put_loc does, and for the same
                       reason: without it this assignment leaves a slot number in
                       the variable that the next one will not match, and the two
                       meet at a jump target as different types. The value also
                       stays on the stack, so both places have to name the same
                       slot. */
                    int src = WHALE_SLOT(t);

                    if (loc_slot[idx] < 0) {
                        if (whale_objs >= WHALE_CALL_BASE)
                            goto out;
                        loc_slot[idx] = whale_objs++;
                    }
                    if (src != loc_slot[idx]) {
                        fm = fold_mark();
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjSlotCopy"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(src); wt(','); wt_int(loc_slot[idx]); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    local_forget(idx);
                    cur[idx] = WHALE_VAL_T(loc_slot[idx]);
                    cur[var_count + sp - 1] = WHALE_VAL_T(loc_slot[idx]);
                    lo_c[idx] = 0;                    fm = fold_mark();
                    wt_id("_ws"); wt('.'); wt_id("fver"); wt('=');
                    wt_id("_ws"); wt('.'); wt_id("fver"); wt_op(JS_OP_ADD); wt_int(1); wt(';');
                    fold_commit(fm);
                    break;
                }
                local_forget(idx);
                fm = fold_mark();
                fold_inline(sp - 1);
                local_set(idx, fm);
                cur[idx] = (uint8_t)t;
                lo_c[idx] = 0;
                /* sp unchanged */
            }
            break;
        /* add_loc is the fused `L += x` the parser emits for compound
           assignment to a local. */
        case OP_add_loc:
            {
                int idx = (int)pc[pos + 1];
                int fm;

                t = cur[var_count + sp - 1];
                if (t == WHALE_T_UNKNOWN || idx < 0 || idx >= var_count)
                    goto out;
                if (!whale_type_is_num(cur[idx]) || !whale_type_is_num(t))
                    goto out;
                /* Folds to one expression as well. The left operand is the
                   current value, which may itself still be pending, so that is
                   written out first - before the new range is marked. */
                local_materialize(idx);
                fm = fold_mark();
                wt_slot("L", idx); wt('+'); fold_inline(sp - 1);
                local_set(idx, fm);
                cur[idx] = WHALE_T_NUM;
                lo_c[idx] = 0;
                sp--;
            }
            break;
        case OP_get_loc0_loc1:
            if (var_count < 2 || cur[0] == WHALE_T_UNKNOWN || cur[1] == WHALE_T_UNKNOWN)
                goto out;
            {
                int fm = fold_mark();

                local_inline(0);
                fold_set(sp, fm);
                fm = fold_mark();
                local_inline(1);
                fold_set(sp + 1, fm);
            }
            cur[var_count + sp] = cur[0];
            cur[var_count + sp + 1] = cur[1];
            sp += 2;
            break;
        case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
        case OP_get_arg:
            {
                int idx = (op == OP_get_arg) ? (int)whale_get_u16(pc + pos + 1) : op - OP_get_arg0;
                int fm;

                if (idx < 0 || idx >= arg_count)
                    goto out;
                if (arg_obj_slot[idx] >= 0) {
                    /* This argument is the receiver of a property access, so it
                       travels as the JSValue parked in its slot. */
                    cur[var_count + sp] = WHALE_VAL_T(arg_obj_slot[idx]);
                    whale_vmat[sp] = 1;
                    sp++;
                    break;
                }
                fm = fold_mark();
                wt_slot("A", idx);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;

        /* closure reference: accepted only when it turns out to be this very
           function at run time, i.e. a direct self-recursive reference */
        case OP_get_var_ref0: case OP_get_var_ref1: case OP_get_var_ref2: case OP_get_var_ref3:
        case OP_get_var_ref:
            {
                int idx = (op == OP_get_var_ref) ? (int)whale_get_u16(pc + pos + 1) : op - OP_get_var_ref0;
                /* A slot used as a call target is checked once at the entry to
                   be this very function (self-recursion). Any other use is an
                   enclosing closure's variable, read here as a number. */
                if (!whale_var_ref_feeds_call(pc, len, pos, sz)) {
                    int nm = whale_vserial++;
                    int fm = fold_mark();

                    /* The result now depends on something other than the
                       arguments - the captured state - so the engine must not
                       memoise this function. Without this a counter closure
                       returns the first answer for ever. */
                    whale_gen_pure = 0;

                    wk(JS_WK_IF); wt('('); wt('!');
                    wt_id("JS_WhaleVarRefGetNum"); wt('('); wt_id("ctx"); wt(',');
                    wt_id("var_refs"); wt(','); wt_int(idx); wt(','); wt('&'); wt_arr("v", nm); wt(')');
                    wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;
                    cur[var_count + sp] = WHALE_T_NUM;
                    sp++;
                    break;
                }
                if (!whale_assume_self) {
                    /* The other reading of the same bytecode: the slot holds an
                       enclosing closure's value. Park it as-is and let the call
                       site hand it to the engine. */
                    int slot = whale_objs++;
                    int fmb;

                    if (slot >= WHALE_CALL_BASE)
                        goto out;
                    whale_gen_pure = 0;   /* depends on captured state */
                    fmb = fold_mark();
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleVarRefGetObj"); wt('(');
                    wt_id("ctx"); wt(','); wt_id("var_refs"); wt(','); wt_int(idx); wt(',');
                    wt_int(slot); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fmb);
                    cur[var_count + sp] = WHALE_VAL_T(slot);
                    sp++;
                    break;
                }
                (void)idx;
                cur[var_count + sp] = WHALE_T_SELF;
                /* SELF has no value expression: mark the slot materialised so
                   a later use can never inline whatever was recorded there
                   before (the slot is only ever consumed as a call target). */
                whale_vmat[sp] = 1;
                sp++;
            }
            break;

        /* writing an enclosing closure's variable */
        case OP_put_var_ref0: case OP_put_var_ref1: case OP_put_var_ref2: case OP_put_var_ref3:
        case OP_put_var_ref:
            {
                int idx = (op == OP_put_var_ref) ? (int)whale_get_u16(pc + pos + 1)
                                                 : op - OP_put_var_ref0;
                int fm;

                if (!whale_type_is_num(cur[var_count + sp - 1]))
                    goto out;
                whale_gen_pure = 0;   /* writes captured state */
                fm = fold_mark();
                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleVarRefSetNum"); wt('(');
                wt_id("ctx"); wt(','); wt_id("var_refs"); wt(','); wt_int(idx); wt(',');
                fold_inline(sp - 1); wt(')'); wt(')');
                wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                fold_commit(fm);
                sp--;
            }
            break;

        /* A global read. The value is fetched through a helper that also checks
           it is a number, because the generated code cannot deal with anything
           else - a non-number bails out (deopt) and the interpreter runs the
           frame instead, which is exactly what the entry unboxing does for
           arguments. */
        case OP_get_var:
        case OP_get_var_undef:
            {
                int atom = (int)whale_get_u32(pc + pos + 1);
                int fm = fold_mark();


                /* Reading a global makes the result depend on something other
                   than the arguments, so the engine must not memoise this
                   function. */
                whale_gen_pure = 0;
                if (whale_feeds_call_as_callee(pc, len, pos, sz)) {
                    /* The value is about to be called, so it has to travel as
                       the JSValue it is. A slot is the only way the generated
                       code can hold something that is not a number. A wrong
                       guess here costs the optimisation, never correctness:
                       nothing but a call accepts WHALE_T_VAL. */
                    int slot = whale_objs++;
                    int fm2;

                    if (slot >= WHALE_CALL_BASE)
                        goto out;
                    fm2 = fold_mark();
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleGetGlobalObj"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(atom); wt(','); wt_int(slot); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm2);
                    cur[var_count + sp] = WHALE_VAL_T(slot);
                    sp++;
                    break;
                }
                if (JS_WhaleGlobalIsU8Ctor(ctx, atom)) {
                    /* `new Uint8Array(...)` needs the constructor itself kept.
                       Judged here, while emitting, so the slot's identity is a
                       single compile-time fact - see the WHALE_OBJ_T note. */
                    int slot = whale_objs++;
                    int nm;

                    if (slot >= WHALE_CALL_BASE)
                        goto out;
                    nm = whale_vserial++;
                    wk(JS_WK_IF); wt('('); wt('!');
                    wt_id("JS_WhaleGetGlobalObj"); wt('('); wt_id("ctx"); wt(','); wt_int(atom); wt(','); wt_int(slot); wt(')');
                    wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;   /* marks the fold slot dead; unused for objects */
                    cur[var_count + sp] = WHALE_OBJ_T(slot);
                    sp++;
                    break;
                }
                {
                    int nm = whale_vserial++;   /* fresh name, never reused */
                    wk(JS_WK_IF); wt('('); wt('!');
                    wt_id("JS_WhaleGetGlobalNum"); wt('('); wt_id("ctx"); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("v", nm); wt(')');
                    wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    /* the helper wrote straight into the slot, so it is a real
                       variable now rather than a folded expression */
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;
                }
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;

        /* self-recursive call */
        case OP_call0: case OP_call1: case OP_call2: case OP_call3:
        case OP_call:
            {
                int argc = (op == OP_call) ? (int)whale_get_u16(pc + pos + 1) : op - OP_call0;
                int func_slot = sp - 1 - argc;
                int k, fm;

                if (func_slot < 0)
                    goto out;
                if (cur[var_count + func_slot] == WHALE_T_FN && !whale_inline_nested) {
                    /* A callee this compilation can see through: expand it in
                       place instead of crossing into the engine. */
                    JSWhaleFuncInfo ci;

                    if (JS_WhaleCpoolFuncInfo(ctx, info->cpool, whale_slot_fn[func_slot], &ci)) {
                        int rtype, saved_nested = whale_inline_nested;

                        whale_inline_nested = 1;
                        for (k = 0; k < argc; k++) {
                            int tk = cur[var_count + sp - argc + k];

                            if (!whale_type_is_num(tk) && !WHALE_IS_SLOT(tk)) {
                                whale_inline_nested = saved_nested;
                                goto whale_call_fallback;
                            }
                        }
                        fm = fold_mark();
                        rtype = whale_inline_body(&ci, cur, var_count, sp - argc);
                        whale_inline_nested = saved_nested;
                        if (getenv("WHALE_JIT_DUMP"))
                            fprintf(stderr, "[inline] pos=%d argc=%d rtype=%d (num=%d slot=%d obj=%d)\n",
                                    pos, argc, rtype, rtype == WHALE_T_NUM,
                                    WHALE_IS_SLOT(rtype), WHALE_IS_OBJ(rtype));
                        if (rtype < 0) {
                            /* the inliner could not expand this callee - it has
                               locals, or holds something the inliner does not
                               handle. Throw away whatever it managed to emit and
                               take the general call path instead of abandoning the
                               whole function. */
                            JS_WhaleTokSinkTruncate(fm);
                            goto whale_call_fallback;
                        }
                        fold_commit(fm);
                        cur[var_count + func_slot] = (uint8_t)rtype;
                        sp = func_slot + 1;
                        break;
                    }
                }
                /* Inlining did not apply: the callee has locals, holds something
                   the inliner does not handle, or takes an argument that is not a
                   number or a slot. Abandoning the function here is what kept
                   immutable_update and vnode_build out of the JIT entirely. The
                   constant pool entry is a real JSFunction, so the general call
                   path below can take it - move it into a slot and re-type this
                   entry so that path accepts it. */
            whale_call_fallback:
                if (cur[var_count + func_slot] == WHALE_T_FN) {
                    int fs;

                    if (whale_objs >= WHALE_CALL_BASE)
                        goto out;
                    fs = whale_objs++;
                    {
                        int fm = fold_mark();

                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleCpoolSlot"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(whale_slot_fn[func_slot]); wt(',');
                        wt_int(fs); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    cur[var_count + func_slot] = WHALE_VAL_T(fs);
                }
                if (cur[var_count + func_slot] != WHALE_T_SELF) {
                    /* Not this function, so it is a host call - a DOM write, a
                       logging call. Compile it as a single call into the engine
                       and carry on, which is what stops a function that is
                       mostly arithmetic with one host call at the end from
                       falling back to the interpreter entirely.

                       Only the shape where the result is dropped is accepted.
                       Keeping a result would mean holding a JSValue the
                       generated code cannot do arithmetic on, and asking for a
                       number later would deopt - re-running the frame, and with
                       it the call. A DOM mutation must not happen twice, so a
                       result we cannot use is a result we refuse to have; the
                       drop that follows in the bytecode confirms it is dead. */
                    int tc = cur[var_count + func_slot];
                    int nxt = pos + sz;
                    int ret_slot;

                    if (getenv("WHALE_JIT_DUMP"))
                        fprintf(stderr, "[call] pos=%d argc=%d tc=%d slot=%d callslots=%d\n",
                                pos, argc, tc, WHALE_IS_SLOT(tc), WHALE_CALL_SLOTS);
                    if (!WHALE_IS_SLOT(tc))
                        goto out;
                    if (argc > WHALE_CALL_SLOTS)
                        goto out;
                    /* Either the result is dead - the bytecode drops it right
                       after - or it is parked in a slot as an opaque value. A
                       result that is neither is refused: needing a number from
                       it later would deopt, and re-running this frame would run
                       the call a second time. */
                    if (nxt < len && (int)pc[nxt] == OP_drop) {
                        ret_slot = -1;
                    } else {
                        if (whale_objs >= WHALE_CALL_BASE)
                            goto out;
                        ret_slot = whale_objs++;
                    }
                    for (k = 0; k < argc; k++) {
                        int tk = cur[var_count + sp - argc + k];

                        if (!whale_type_is_num(tk) && !WHALE_IS_VAL(tk) && !WHALE_IS_OBJ(tk)) {
                            if (getenv("WHALE_JIT_DUMP"))
                                fprintf(stderr, "[call] arg k=%d tk=%d rejected\n", k, tk);
                            goto out;
                        }
                    }
                    fm = fold_mark();
                    for (k = 0; k < argc; k++) {
                        int tk = cur[var_count + sp - argc + k];

                        wk(JS_WK_IF); wt('('); wt('!');
                        if (whale_type_is_num(tk)) {
                            wt_id("JS_WhaleObjSlotNum"); wt('('); wt_id("ctx"); wt(',');
                            wt_int(WHALE_CALL_BASE + k); wt(',');
                            fold_inline(sp - argc + k); wt(')');
                        } else {
                            wt_id("JS_WhaleObjSlotCopy"); wt('('); wt_id("ctx"); wt(',');
                            wt_int(WHALE_IS_VAL(tk) ? WHALE_VAL_SLOT(tk) : WHALE_OBJ_SLOT(tk)); wt(',');
                            wt_int(WHALE_CALL_BASE + k); wt(')');
                        }
                        wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    }
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleCallValue"); wt('(');
                    wt_id("ctx"); wt(',');
                    wt_int(WHALE_IS_VAL(tc) ? WHALE_VAL_SLOT(tc) : WHALE_OBJ_SLOT(tc)); wt(',');
                    wt_int(argc); wt(','); wt_int(WHALE_CALL_BASE); wt(',');
                    wt_int(ret_slot); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(2); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    cur[var_count + func_slot] = (ret_slot < 0)
                        ? (uint8_t)WHALE_T_UNKNOWN : WHALE_VAL_T(ret_slot);
                    sp = func_slot + 1;
                    break;
                }
                for (k = 0; k < argc; k++) {
                    if (!whale_type_is_num(cur[var_count + sp - argc + k]))
                        goto out;
                }
                fm = fold_mark();
                wt('{'); wk(JS_WK_DOUBLE); wt_id("_an"); wt('['); wt_int(argc > 0 ? argc : 1); wt(']'); wt(';'); wk(JS_WK_DOUBLE); wt_id("_r"); wt('='); wt_int(0); wt(';'); wk(JS_WK_INT); wt_id("_dd"); wt('='); wt_int(0); wt(';');
                for (k = 0; k < argc; k++) {
                    wt_id("_an"); wt('['); wt_int(k); wt(']'); wt('='); fold_inline(sp - argc + k); wt(';');
                }
                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleCheckStack"); wt('('); wt_id("ctx"); wt(')'); wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                wt_id("_r"); wt('='); wt_id(whale_core_name); wt('('); wt_id("ctx"); wt(','); wt_id("args"); wt(','); wt_id("_an"); wt(','); wt('&'); wt_id("_dd"); wt(','); wt('&'); wt_id("_cref"); wt(','); wt_id("var_refs"); wt(')'); wt(';');
                wk(JS_WK_IF); wt('('); wt_id("_dd"); wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                {
                    int nm = whale_vserial++;   /* fresh name, never reused */
                    wt_arr("v", nm); wt('='); wt_id("_r"); wt(';'); wt('}');
                    fold_commit(fm);
                    /* The result is in that variable now. Marking it
                       materialised is what makes later uses refer to it: without
                       this the slot still held the expression of some earlier
                       value (a call argument, say) and would be inlined as that
                       instead of as the call result. */
                    whale_vname[func_slot] = nm;
                    whale_vmat[func_slot] = 1;
                }
                cur[var_count + func_slot] = WHALE_T_NUM;
                sp = func_slot + 1;
            }
            break;

        case OP_add: case OP_sub: case OP_mul: case OP_div:
        case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
            {
                int is_cmp = 0;
                int opid = whale_binop_op(op, &is_cmp);

                ta = cur[var_count + sp - 2];
                tb = cur[var_count + sp - 1];
                if (opid < 0)
                    goto out;
                if (!whale_type_is_num(ta) || !whale_type_is_num(tb)) {
                    /* One operand is opaque - a string, a null, a value from a call.
                       That used to abandon the whole function; the engine can do the
                       operation properly, so hand it over and carry on. This is what
                       lets a vnode record, whose key is a string, stay compiled. */
                    {
                        int _okv;

                        WHALE_EMIT_BINOP_VAL(op, ta, tb, sp - 2, sp - 1, _okv);
                        if (!_okv)
                            goto out;
                    }
                    sp--;
                    break;
                }
                if (is_cmp) {
                    int fm = fold_mark();

                    wt('('); fold_inline(sp - 2); wt_op(opid); fold_inline(sp - 1); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0);
                    fold_set(sp - 2, fm);
                    cur[var_count + sp - 2] = WHALE_T_BOOL;
                } else {
                    int fm = fold_mark();

                    fold_inline(sp - 2); wt_op(opid); fold_inline(sp - 1);
                    fold_set(sp - 2, fm);
                    cur[var_count + sp - 2] = WHALE_T_NUM;
                }
                sp--;
            }
            break;
        case OP_mod:
            ta = cur[var_count + sp - 2];
            tb = cur[var_count + sp - 1];
            if (!whale_type_is_num(ta) || !whale_type_is_num(tb))
                goto out;
            {
                int fm;
                int pow2 = 0;

                /* A divisor that is a compile-time power of two lets the whole
                   modulo become a mask. The value is readable the same way
                   fold_toint32 reads an iconst: a single-token expression that is
                   not a named variable is a literal, and its value sits in gw_num.
                   This is the shape `(a + i * 3) % 65536` has. */
                if (tb == WHALE_T_ICONST && !whale_vmat[sp - 1] &&
                    whale_vexpr[sp - 1].len == 1) {
                    double bv = gw_num[whale_vexpr[sp - 1].off];

                    if (bv > 0 && bv <= 2147483648.0 && bv == (double)(long long)bv &&
                        (((long long)bv & ((long long)bv - 1)) == 0))
                        pow2 = (int)bv;
                }

                /* The form fold_mod emits mentions each operand up to four times
                   (two range tests, the fast path, the fmod fallback), so give
                   both names first: inlining a pending expression into all those
                   copies copies it, copies of copies included. One `%` in a loop
                   generated kilobytes of C that way. This has to happen before
                   fold_mark(): materialising appends statements, and anything
                   inside the marked range would be treated as part of the
                   expression. */
                if (!whale_vmat[sp - 2])
                    fold_materialize(sp - 2);
                if (!whale_vmat[sp - 1])
                    fold_materialize(sp - 1);
                fm = fold_mark();
                if (pow2 > 0) {
                    /* `% 2^k` is `& (2^k - 1)`, and idiv is the slowest integer
                       instruction there is - about 26 cycles against 1. The
                       reduction only holds for a non-negative dividend: JS's %
                       takes the sign of the dividend (-1 % 4 is -1) while the mask
                       does not (-1 & 3 is 3), and -0 stays -0 rather than 0. So
                       test `> 0` and leave fmod on the cold branch, exactly the
                       guard fold_mod already uses.
                       Measured before this: (a + i*3) % 65536 ran 113ms against
                       node's 11ms. V8 reaches 11ms by doing precisely this, so the
                       idiv was the whole gap - there was no boxing left to remove.
                       Plain C shows the same thing: guarded idiv 1776ms, bare idiv
                       1781ms, fmod 3028ms per 2e8 operations. The guard is free. */
                    wt('('); fold_inline(sp - 2); wt_op(JS_OP_GT); wt_int(0); wt(')');
                    wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')');
                    wt('('); wt('('); wk(JS_WK_INT); wt(')'); fold_inline(sp - 2);
                    wt('&'); wt_int(pow2 - 1); wt(')');
                    wt(':'); wt_id("fmod"); wt('('); fold_inline(sp - 2);
                    wt(','); wt_int(pow2); wt(')');
                } else {
                    fold_mod(sp - 2, sp - 1);
                }
                fold_set(sp - 2, fm);
            }
            cur[var_count + sp - 2] = WHALE_T_NUM;
            sp--;
            break;
        /* Bitwise operators are defined on int32, so both operands go through
           ToInt32 and the result comes back into the double domain. The shift
           counts are taken mod 32, as JS specifies (a plain C shift by >= 32
           is undefined). */
        case OP_and: case OP_or: case OP_xor:
            ta = cur[var_count + sp - 2];
            tb = cur[var_count + sp - 1];
            if (!whale_type_is_num(ta) || !whale_type_is_num(tb))
                goto out;
            {
                int fm = fold_mark();

                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('(');
                fold_toint32(sp - 2, cur[var_count + sp - 2] == WHALE_T_I32,
                     cur[var_count + sp - 2] == WHALE_T_ICONST);
                wt(op == OP_and ? '&' : op == OP_or ? '|' : '^');
                fold_toint32(sp - 1, cur[var_count + sp - 1] == WHALE_T_I32,
                         cur[var_count + sp - 1] == WHALE_T_ICONST);
                wt(')');
                fold_set(sp - 2, fm);
            }
            cur[var_count + sp - 2] = WHALE_T_I32;   /* & | ^ are int32 by definition */
            sp--;
            break;
        case OP_shl: case OP_shr: case OP_sar:
            ta = cur[var_count + sp - 2];
            tb = cur[var_count + sp - 1];
            if (!whale_type_is_num(ta) || !whale_type_is_num(tb))
                goto out;
            {
                int fm = fold_mark();

                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('(');
                if (op == OP_shr) { wk(JS_WK_UNSIGNED); wk(JS_WK_INT); wt(')'); }   /* >>> is logical */
                fold_toint32(sp - 2, cur[var_count + sp - 2] == WHALE_T_I32,
                     cur[var_count + sp - 2] == WHALE_T_ICONST);
                /* one token, not two '<' - there is no lexer to join them */
                wt_op(op == OP_shl ? JS_OP_SHL : JS_OP_SAR);
                wt('('); fold_toint32(sp - 1, cur[var_count + sp - 1] == WHALE_T_I32,
                         cur[var_count + sp - 1] == WHALE_T_ICONST); wt('&'); wt_int(31); wt(')');
                wt(')');
                fold_set(sp - 2, fm);
            }
            /* >>> is unsigned, so its result can exceed int32; << and >> cannot */
            cur[var_count + sp - 2] = (op == OP_shr) ? WHALE_T_NUM : WHALE_T_I32;
            sp--;
            break;
        case OP_neg:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            {
                int fm = fold_mark();

                wt('-'); fold_inline(sp - 1);
                fold_set(sp - 1, fm);
            }
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        case OP_plus:
            /* +x on a number is the identity */
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        /* QuickJS has two different operators here: `lnot` is the logical
           `!x`, while `not` is the bitwise `~x`. Treating `not` as `!x` made
           every `~x` in a JIT'd function return 0 or 1. */
        case OP_lnot:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            {
                int fm = fold_mark();

                /* !x is true for 0 and for NaN */
                wt('('); wt('('); fold_inline(sp - 1); wt_op(JS_OP_NE); fold_inline(sp - 1); wt(')');
                wt_op(JS_OP_OR); wt('('); fold_inline(sp - 1); wt_op(JS_OP_EQ); wt_int(0); wt(')'); wt(')');
                wt('?'); wt_int(1); wt(':'); wt_int(0);
                fold_set(sp - 1, fm);
            }
            cur[var_count + sp - 1] = WHALE_T_BOOL;
            break;
        case OP_not:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            {
                int fm = fold_mark();

                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('~'); fold_toint32(sp - 1, cur[var_count + sp - 1] == WHALE_T_I32,
                         cur[var_count + sp - 1] == WHALE_T_ICONST);
                fold_set(sp - 1, fm);
            }
            cur[var_count + sp - 1] = WHALE_T_I32;
            break;
        case OP_inc: case OP_dec:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            {
                int fm = fold_mark();

                fold_inline(sp - 1); wt(op == OP_inc ? '+' : '-'); wt_int(1);
                fold_set(sp - 1, fm);
            }
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        case OP_inc_loc:
            {
                int idx = (int)pc[pos + 1];
                int fm;

                if (idx < 0 || idx >= var_count || !whale_type_is_num(cur[idx]))
                    goto out;
                /* `L++` folds like add_loc: the current value may still be a
                   pending expression, so it is written out first. */
                local_materialize(idx);
                fm = fold_mark();
                wt_slot("L", idx); wt('+'); wt_int(1);
                local_set(idx, fm);
                cur[idx] = WHALE_T_NUM;
                lo_c[idx] = 0;
            }
            break;
        case OP_drop:
            if (sp <= 0)
                goto out;
            /* the value just disappears; its expression is never inlined and
               the slot is overwritten by the next push */
            sp--;
            break;
        case OP_dup:
            if (sp <= 0)
                goto out;
            /* both slots share the expression. It has no side effects, so
               inlining it twice is fine. */
            whale_vexpr[sp] = whale_vexpr[sp - 1];
            whale_vmat[sp] = whale_vmat[sp - 1];
            cur[var_count + sp] = cur[var_count + sp - 1];
            sp++;
            break;

        case OP_if_false8: case OP_if_true8: case OP_if_false: case OP_if_true:
            {
                int is8 = (op == OP_if_false8 || op == OP_if_true8);
                int is_true = (op == OP_if_true8 || op == OP_if_true);
                int tgt = is8 ? (pos + 1 + (int8_t)pc[pos + 1])
                              : (pos + 1 + (int32_t)whale_get_u32(pc + pos + 1));
                int fm;

                t = cur[var_count + sp - 1];
                if (!whale_type_is_num(t))
                    goto out;
                fm = fold_mark();
                wk(JS_WK_IF); wt('('); fold_inline(sp - 1); wt_op(is_true ? JS_OP_NE : JS_OP_EQ); wt_int(0); wt(')'); wk(JS_WK_GOTO); wt_slot("L", tgt); wt(';');
                fold_commit(fm);
                sp--;
                /* the edge leaving this jump defines the target state */
                if ((tgt >= 0) && (tgt <= len) && (!seen[tgt])) {
                    cur[state_len - 1] = (uint8_t)sp;
                    seen[tgt] = 1;
                    memcpy(snap + (size_t)tgt * state_len, cur, state_len);
                }
            }
            break;
        case OP_goto8: case OP_goto:
            {
                int tgt = (op == OP_goto8) ? (pos + 1 + (int8_t)pc[pos + 1])
                                           : (pos + 1 + (int32_t)whale_get_u32(pc + pos + 1));
                int fm = fold_mark();

                wk(JS_WK_GOTO); wt_slot("L", tgt); wt(';');
                fold_commit(fm);
                unreachable = 1;
                if ((tgt >= 0) && (tgt <= len) && (!seen[tgt])) {
                    cur[state_len - 1] = (uint8_t)sp;
                    seen[tgt] = 1;
                    memcpy(snap + (size_t)tgt * state_len, cur, state_len);
                }
            }
            break;
        case OP_return:
            {
                int fm;

                t = cur[var_count + sp - 1];
                if (WHALE_IS_ARR(t) || WHALE_IS_OBJ(t)) {
                    /* The slot holds the reference; hand a duplicate of it to
                       the caller through ret_obj so the two do not compete for
                       the same reference count. A function that returns both a
                       number and an object is refused - one return shape only. */
                    if (ret_is_bool != -1)
                        goto out;
                    whale_ret_obj = 1;
                    fm = fold_mark();
                    wt('*'); wt_id("ret_obj"); wt('='); wt_id("JS_WhaleObjGet"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(t)); wt(')'); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';');
                    fold_commit(fm);
                    unreachable = 1;
                    break;
                }
                if (!whale_type_is_num(t))
                    goto out;
                if (whale_ret_obj)
                    goto out; /* a numeric return after an object one: mixed */
                if (ret_is_bool == -1)
                    ret_is_bool = (t == WHALE_T_BOOL);
                else if (ret_is_bool != (t == WHALE_T_BOOL))
                    goto out; /* mixed number/boolean returns: not our shape */
                fm = fold_mark();
                wk(JS_WK_RETURN); fold_inline(sp - 1); wt(';');
                fold_commit(fm);
                unreachable = 1;
            }
            break;
        default:
            /* Nothing here lowers this opcode. Hand the single instruction to
               the engine and keep the rest of the function native - see
               whale_step_emit. If even that cannot carry it, this function
               keeps interpreting, as before. */
            if (whale_step_emit(pc[pos], pos, &sp, cur, var_count, stack_size))
                break;
            goto out; /* anything else: keep interpreting */
        }
        pos += sz;

        /* the back edge of the innermost open loop closes its buffer */
        if (whale_loop_depth > 0 &&
            pos == whale_loops[whale_loop_depth - 1].back_pos)
            loop_end();
    }

    /* a loop without a back edge (a `for(;;)` whose exit is a throw, say) would
       leave its buffer open - flush whatever is left before committing */
    while (whale_loop_depth > 0)
        loop_end();

    /* The epilogue and the whole engine entry are structural, so they are
       committed as one block rather than folded. */
    tail_mark = fold_mark();
    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';');
    wt('}');
    /* engine entry: unbox the arguments once, run core, box the result */
    wt_id("JSValue"); wt_id(whale_entry_name); wt('('); wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt('*'); wt_id("args"); wt(','); wt_id("JSValue"); wt('*'); wt_id("var_buf"); wt(','); wk(JS_WK_INT); wt('*'); wt_id("deopt"); wt(','); wt_id("JSValue"); wt_id("whale_self"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("whale_entry"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("whale_var_refs"); wt(')'); wt('{');
    wk(JS_WK_DOUBLE); wt_id("An"); wt('['); wt_int(arg_count > 0 ? arg_count : 1); wt(']'); wt(';'); wk(JS_WK_DOUBLE); wt_id("_r"); wt(';'); wt_id("JSValue"); wt_id("_ret"); wt(';'); wt_id("_ret"); wt('.'); wt_id("tag"); wt('='); wt_int(3); wt(';');
    /* On any failure set *deopt and return 0: the caller checks the flag first
       and interprets the frame itself, so the returned JSValue is discarded.
       That keeps JS_EXCEPTION (a macro with no linkable address) out of the
       generated code entirely. */
    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleCheckStack"); wt('('); wt_id("ctx"); wt(')'); wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';'); wt('}');
    {
        /* ---- object-slot self test (WHALE_OBJ_TEST=<arg index>) -----------
           Park the argument in a JIT object slot, force a full GC, and return
           its .length. This exists to prove the slot mechanism: if the frame's
           live range did not really cover the slots, the collection below frees
           the array and this reads freed memory instead of a length. Only built
           when the variable is set, so normal code is untouched. */
        const char *ots = getenv("WHALE_OBJ_TEST");
        int oti = ots ? atoi(ots) : -1;

        if (oti >= 0 && oti < arg_count) {
            wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(',');
            wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_id("JS_WhaleObjProbeFrom"); wt('(');
            wt_id("ctx"); wt(','); wt_id("args"); wt('['); wt_int(oti); wt(']'); wt(')');
            wt(')'); wt(';');
        }
    }
    for (i = 0; i < arg_count; i++) {
        if (!args_used[i])
            continue;
        if (arg_obj_slot[i] >= 0) {
            /* travels as an object, so it is not unboxed as a number */
            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleArgGetObj"); wt('('); wt_id("ctx"); wt(',');
            wt_id("args"); wt('['); wt_int(i); wt(']'); wt(','); wt_int(arg_obj_slot[i]); wt(')'); wt(')');
            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
            wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';'); wt('}');
            continue;
        }
        wk(JS_WK_IF); wt('('); wt('!'); wt_id("whale_jit_unbox_num"); wt('('); wt_id("ctx"); wt(','); wt_id("args"); wt('['); wt_int(i); wt(']'); wt(','); wt('&'); wt_id("An"); wt('['); wt_int(i); wt(']'); wt(')'); wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';'); wt('}');
    }
    /* closure slots we rely on must be this very function (once per entry;
       the closure cannot change during the call, so recursion need not re-check) */
    for (i = 0; i < 4; i++) {
        if (var_ref_used[i]) {
            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleVarRefIsSelf"); wt('('); wt_id("ctx"); wt(','); wt_id("whale_var_refs"); wt(','); wt_int(i); wt(','); wt_id("whale_self"); wt(')'); wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(3); wt(';'); wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';'); wt('}');
        }
    }
    wt_id("_r"); wt('='); wt_id(whale_core_name); wt('('); wt_id("ctx"); wt(','); wt_id("args"); wt(','); wt_id("An"); wt(','); wt_id("deopt"); wt(','); wt('&'); wt_id("_ret"); wt(','); wt_id("whale_var_refs"); wt(')'); wt(';');
    /* *deopt == 2 is not "reinterpret this frame", it is "a host call inside
       the compiled body threw". The exception is already pending in ctx and the
       callee must not run a second time - it is exactly the kind of thing (a
       DOM mutation) that is not safe to repeat - so hand the exception back.
       Clearing the flag keeps the engine's deopt path out of it. */
    wk(JS_WK_IF); wt('('); wt('*'); wt_id("deopt"); wt_op(JS_OP_EQ); wt_int(2); wt(')'); wt('{');
    wt('*'); wt_id("deopt"); wt('='); wt_int(0); wt(';'); wk(JS_WK_RETURN); wt_id("JS_WhaleExceptionValue"); wt('('); wt_id("ctx"); wt(')'); wt(';'); wt('}');
    wk(JS_WK_IF); wt('('); wt('*'); wt_id("deopt"); wt(')'); wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';');
    /* An array or object result came back through ret_obj instead. Only a
       function that can actually return one gets the check - a numeric one
       should not pay a comparison per call. */
    if (whale_ret_obj) {
        wk(JS_WK_IF); wt('('); wt_id("_ret"); wt('.'); wt_id("tag"); wt_op(JS_OP_NE); wt_int(3); wt(')');
        wk(JS_WK_RETURN); wt_id("_ret"); wt(';');
    }
    if (ret_is_bool == 1) {
        wk(JS_WK_RETURN); wt_id("JS_NewBool"); wt('('); wt_id("ctx"); wt(','); wt_id("_r"); wt_op(JS_OP_NE); wt_int(0); wt(')'); wt(';');
    } else {
        wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_id("_r"); wt(')'); wt(';');
    }
    wt('}');
    fold_commit(tail_mark);
    /* hand the linearised stream to the parser */
    JS_WhaleTokCommit(go_tok, go_num, go_len);
    ret = 1; /* every opcode lowered successfully */
    whale_gen_objs = whale_objs;
  out:
    if (!ret && getenv("WHALE_JIT_DUMP")) {
        int bad = (pos < len) ? pc[pos] : -1;

        fprintf(stderr, "whale jit: gen gave up at pos=%d op=%d (%s)\n",
                pos, bad, whale_op_name(bad));
    }
    if (targets)
        free(targets);
    if (seen)
        free(seen);
    if (snap)
        free(snap);
    if (cur)
        free(cur);
    if (args_used)
        free(args_used);
    if (args_obj)
        free(args_obj);
    if (arg_obj_slot)
        free(arg_obj_slot);
    if (loc_slot)
        free(loc_slot);
    if (loop_back)
        free(loop_back);
    if (assign_cnt)
        free(assign_cnt);
    if (whale_vexpr)
        free(whale_vexpr);
    if (whale_vmat)
        free(whale_vmat);
    if (whale_lmem)
        free(whale_lmem);
    whale_vexpr = NULL;
    whale_vmat = NULL;
    whale_lmem = NULL;
    return ret;
}

/* =========================================================================
 * WhaleJS: JIT/AOT backend interface
 *
 * Everything above produces C source for a function; everything here turns that
 * source into a callable entry point. Today the only backend is TinyCC, which
 * compiles in-process into memory. A future AOT path (offline gcc/clang with an
 * on-disk cache, per rules/goal.md) plugs in by implementing this struct and
 * pointing whale_jit_backend at it - no other code has to change.
 * ========================================================================= */

typedef struct JSWhaleJitBackend {
    const char *name;
    /* Returns 1 on success and fills entry_out (the callable) plus handle_out
       (an opaque object that must stay alive as long as the entry is used). */
    int (*compile)(JSContext *ctx, const char *src, void **entry_out, void **handle_out);
    void (*release)(void *handle);
} JSWhaleJitBackend;

/* The generator runs inside JS_WhaleCompileTokens(), after the translation unit
   is open - before that tok_alloc() has no token table to allocate from. */
static JSContext *whale_gen_ctx;
static const JSWhaleFuncInfo *whale_gen_info;

static void whale_gen_cb(void *unused)
{
    (void)unused;
    /* Tell the interface whether the generator produced a complete stream. When
       it bails on an opcode it cannot lower, the tokens emitted so far are a
       truncated function; letting TinyCC parse them would report a bogus syntax
       error, so the interface skips the parse instead. */
    whale_gen_ok = whale_jit_gen(whale_gen_ctx, whale_gen_info);
}

static int whale_tcc_compile(JSContext *ctx, const char *src, void **entry_out, void **handle_out)
{
    TCCState *s;
    void *fn;

    if (getenv("WHALE_JIT_DUMP"))
        fprintf(stderr, "whale jit paths: src=%s rt=%s\n", WHALE_JIT_SRC_DIR, WHALE_JIT_TCC_DIR);

    /* the lexer must be OFF before anything in TinyCC defines a builtin macro,
       otherwise tcc_define_symbol() would read our (finished) token stream */
    whale_tok_active = 0;

    /* One TCCState for the whole process. tcc_new() rebuilds global compiler
       state (it releases sym_pools, which every state shares) and there is
       nothing to gain from paying for that per compile: the state is created
       once, every later compile is just another file inside it, and it lives
       until process exit. The generated names carry a counter because the code
       accumulates in this one state. */
    if (!whale_jit_state) {
        s = tcc_new();
        if (!s)
            return 0;

        tcc_set_lib_path(s, WHALE_JIT_TCC_DIR);
        tcc_add_include_path(s, WHALE_JIT_TCC_DIR "/include");
        tcc_add_include_path(s, WHALE_JIT_SRC_DIR);
        tcc_add_library_path(s, WHALE_JIT_TCC_DIR "/lib");
        tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
        tcc_add_symbol(s, "JS_WhaleVarRefIsSelf", (void *)JS_WhaleVarRefIsSelf);
        tcc_add_symbol(s, "JS_WhaleVarRefGetNum", (void *)JS_WhaleVarRefGetNum);
        tcc_add_symbol(s, "JS_WhaleVarRefSetNum", (void *)JS_WhaleVarRefSetNum);
        tcc_add_symbol(s, "JS_WhaleVarRefGetObj", (void *)JS_WhaleVarRefGetObj);
        tcc_add_symbol(s, "JS_WhaleCheckStack", (void *)JS_WhaleCheckStack);
        tcc_add_symbol(s, "whale_jit_box_num", (void *)whale_jit_box_num);
        tcc_add_symbol(s, "whale_jit_unbox_num", (void *)whale_jit_unbox_num);
        tcc_add_symbol(s, "JS_NewFloat64", (void *)JS_NewFloat64);
        tcc_add_symbol(s, "JS_NewBool", (void *)JS_NewBool);
        tcc_add_symbol(s, "JS_WhaleToInt32", (void *)JS_WhaleToInt32);
        tcc_add_symbol(s, "JS_WhaleGetGlobalNum", (void *)JS_WhaleGetGlobalNum);
        /* object slots: the GC-visible place to keep an object across a call */
        tcc_add_symbol(s, "JS_WhaleObjSlot", (void *)JS_WhaleObjSlot);
        tcc_add_symbol(s, "JS_WhaleObjGet", (void *)JS_WhaleObjGet);
        tcc_add_symbol(s, "JS_WhaleObjProbe", (void *)JS_WhaleObjProbe);
        tcc_add_symbol(s, "JS_WhaleObjProbeFrom", (void *)JS_WhaleObjProbeFrom);
        /* typed arrays: the array travels as an object slot number */
        tcc_add_symbol(s, "JS_WhaleGetGlobalObj", (void *)JS_WhaleGetGlobalObj);
        tcc_add_symbol(s, "JS_WhaleNewU8Array", (void *)JS_WhaleNewU8Array);
        tcc_add_symbol(s, "JS_WhaleTAU8Get", (void *)JS_WhaleTAU8Get);
        tcc_add_symbol(s, "JS_WhaleTAU8Set", (void *)JS_WhaleTAU8Set);
        tcc_add_symbol(s, "JS_WhaleTAU8Ptr", (void *)JS_WhaleTAU8Ptr);
        tcc_add_symbol(s, "JS_WhaleObjSlotNull", (void *)JS_WhaleObjSlotNull);
        tcc_add_symbol(s, "JS_WhaleObjSlotAtom", (void *)JS_WhaleObjSlotAtom);
        tcc_add_symbol(s, "JS_WhaleObjDefineValue", (void *)JS_WhaleObjDefineValue);
        tcc_add_symbol(s, "JS_WhaleBinopVal", (void *)JS_WhaleBinopVal);
        tcc_add_symbol(s, "JS_WhaleCpoolSlot", (void *)JS_WhaleCpoolSlot);
        tcc_add_symbol(s, "JS_WhaleObjStoreValue", (void *)JS_WhaleObjStoreValue);
        tcc_add_symbol(s, "JS_WhaleShapeFields", (void *)JS_WhaleShapeFields);
        tcc_add_symbol(s, "JS_WhaleObjNew", (void *)JS_WhaleObjNew);
        tcc_add_symbol(s, "JS_WhaleObjShape", (void *)JS_WhaleObjShape);
        tcc_add_symbol(s, "JS_WhaleObjFieldPtr", (void *)JS_WhaleObjFieldPtr);
        tcc_add_symbol(s, "JS_WhaleObjDefineNum", (void *)JS_WhaleObjDefineNum);
        tcc_add_symbol(s, "JS_WhaleObjValueNum", (void *)JS_WhaleObjValueNum);
        tcc_add_symbol(s, "JS_WhaleArrPtr", (void *)JS_WhaleArrPtr);
        tcc_add_symbol(s, "JS_WhaleArrNew", (void *)JS_WhaleArrNew);
        tcc_add_symbol(s, "JS_WhaleArrSetNum", (void *)JS_WhaleArrSetNum);
        tcc_add_symbol(s, "JS_WhaleObjFieldNum", (void *)JS_WhaleObjFieldNum);
        tcc_add_symbol(s, "JS_WhaleObjStoreNum", (void *)JS_WhaleObjStoreNum);
        tcc_add_symbol(s, "JS_WhaleObjSlotNum", (void *)JS_WhaleObjSlotNum);
        tcc_add_symbol(s, "JS_WhaleObjSlotCopy", (void *)JS_WhaleObjSlotCopy);
        tcc_add_symbol(s, "JS_WhaleArgGetObj", (void *)JS_WhaleArgGetObj);
        tcc_add_symbol(s, "JS_WhaleCallValue", (void *)JS_WhaleCallValue);
        tcc_add_symbol(s, "JS_WhaleExceptionValue", (void *)JS_WhaleExceptionValue);
        /* the single-instruction fallback */
        tcc_add_symbol(s, "JS_WhaleStepPushNum", (void *)JS_WhaleStepPushNum);
        tcc_add_symbol(s, "JS_WhaleStepPushSlot", (void *)JS_WhaleStepPushSlot);
        tcc_add_symbol(s, "JS_WhaleStepRun", (void *)JS_WhaleStepRun);
        tcc_add_symbol(s, "JS_WhaleStepOutIsNum", (void *)JS_WhaleStepOutIsNum);
        tcc_add_symbol(s, "JS_WhaleStepOutNum", (void *)JS_WhaleStepOutNum);
        tcc_add_symbol(s, "JS_WhaleStepOutSlot", (void *)JS_WhaleStepOutSlot);
        tcc_add_symbol(s, "JS_WhaleStepClear", (void *)JS_WhaleStepClear);
        whale_jit_state = s;
    }
    s = whale_jit_state;

    /* fresh names per compile: the code accumulates in this one state */
    /* One JS function becomes two C functions (a core and a wrapper). Pinned
       registers have to span both, so claim the map for the whole compilation
       rather than letting each C function reset it. */
    JS_WhalePinEnter();
    snprintf(whale_core_name, sizeof(whale_core_name), "whale_jit_core_%u", ++whale_jit_seq);
    snprintf(whale_entry_name, sizeof(whale_entry_name), "whale_jit_fn_%u", whale_jit_seq);

    /* Drive TinyCC's parser with the tokens the generator pushed: no source
       text is parsed and no header is read. */
    (void)src;
    if (!JS_WhaleCompileTokens(s, whale_gen_cb, NULL)) {
        /* Which side of the hand-off failed matters: the generator bailing on an
           opcode is a different problem from the tokens not parsing or TinyCC
           failing later. All of them end up here. */
        if (getenv("WHALE_JIT_DUMP"))
            fprintf(stderr, "whale jit: compile failed (gen_ok=%d)\n", whale_gen_ok);
        goto fail;
    }
    /* Diagnostics: write the object out so the machine code TinyCC actually
       produced can be disassembled (objdump -d). Everything so far has been read
       from the generated C, which is one step removed from what runs. */
    if (getenv("WHALE_JIT_OBJ")) {
        char objbuf[256];
        FILE *df;

        /* fresh names per compile, so the sequence number identifies the unit */
        snprintf(objbuf, sizeof(objbuf), "%s/whale_%u.bin", getenv("WHALE_JIT_OBJ"), whale_jit_seq);
        df = fopen(objbuf, "wb");
        if (df) {
            JS_WhaleDumpCode(df);
            fclose(df);
            fprintf(stderr, "whale jit: wrote %s\n", objbuf);
        }
    }
    if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0)
        goto fail;
    fn = tcc_get_symbol(s, whale_entry_name);
    if (!fn)
        goto fail;

    if (getenv("WHALE_JIT_DUMP"))
        fprintf(stderr, "whale jit: %s -> %p\n", whale_entry_name, fn);


    *entry_out = fn;
    *handle_out = s;
    return 1;
  fail:
    /* keep the state: it owns every previously compiled function's code, and
       tcc_delete() would release the global symbol pools they share */
    return 0;
}

static void whale_tcc_release(void *handle)
{
    /* the single TCCState is process-wide and must outlive the functions it
       hosts, so dropping one function's handle must not tear it down */
    (void)handle;
}

static JSWhaleJitBackend whale_backend_tcc = {
    "tcc",
    whale_tcc_compile,
    whale_tcc_release,
};

/* The active backend. Swapping this pointer is the AOT insertion point. */
static JSWhaleJitBackend *whale_jit_backend = &whale_backend_tcc;

/* Public entry: compile one function's bytecode and hand back the entry point. */
int JS_WhaleJitCompileFunction(JSContext *ctx, const JSWhaleFuncInfo *info,
                               void **entry_out, void **handle_out)
{
    void *entry = NULL, *handle = NULL;

    if (!entry_out || !handle_out)
        return 0;
    whale_gen_ctx = ctx;
    whale_gen_info = info;

    if (getenv("WHALE_JIT_DUMP")) {
        /* there is no source text any more: the generator emits tokens straight
           into TinyCC. The probe only shows that we really are inside the
           TinyCC translation unit. */
        fprintf(stderr, "whale tcc internal probe: vstack cap=%d\n", JS_WhaleTccProbe());
    }

    if (!whale_jit_backend->compile(ctx, NULL, &entry, &handle))
        return 0;

    *entry_out = entry;
    *handle_out = handle;
    return 1;
}

void JS_WhaleJitRelease(void *handle)
{
    if (handle)
        whale_jit_backend->release(handle);
}