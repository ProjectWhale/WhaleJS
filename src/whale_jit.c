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

static void wt(int t)
{
    JS_WhaleTokChar(t);          /* punctuation / single-character operators */
}

static void wt_id(const char *name)
{
    JS_WhaleTokIdent2(name);
}

static void wt_slot(const char *prefix, int n)
{
    JS_WhaleTokSlot(prefix, n);  /* L0 / v3 / A2 */
}

static void wt_op(int o)
{
    JS_WhaleTokOp(o);
}

static void wt_int(int v)
{
    JS_WhaleTokInt(v);
}

static void wt_num(double v)
{
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
static int whale_var_ref_feeds_call(const uint8_t *pc, int len, int pos, int sz)
{
    int k = pos + sz;
    int n = 0;

    while (k < len && n < 12) {
        int o = (int)pc[k];

        if (whale_is_call_op(o))
            return 1;
        k += whale_op_size_of(o);
        n++;
    }
    return 0;
}

static int whale_type_is_num(int t)
{
    /* WHALE_T_I32 counts as a number: it only adds the knowledge that the value
       is already an in-range int32. */
    return t == WHALE_T_NUM || t == WHALE_T_BOOL || t == WHALE_T_I32;
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
    if (whale_vmat[k]) {
        wt_slot("v", whale_vname[k]);   /* already a real variable */
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
static void fold_toint32(int k, int is_i32)
{
    int repeat;

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
    wt('(');
    wt('('); fold_inline(k); wt_op(JS_OP_GE); wt_num(-2147483648.0); wt(')');
    wt_op(JS_OP_AND);
    wt('('); fold_inline(k); wt_op(JS_OP_LE); wt_num(2147483647.0); wt(')');
    wt('?'); wt('('); wk(JS_WK_INT); wt(')'); fold_inline(k);
    wt(':'); wt_id("JS_WhaleToInt32"); wt('('); fold_inline(k); wt(')');
    wt(')');
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
        wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_fs", fc); wt(')'); wt('{');
    } else {
        wk(JS_WK_IF); wt('('); wt_slot("_fs", fc); wt_op(JS_OP_NE);
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
        wt('{');
    }
    wt_slot("_fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_slot("_fs", fc); wt(')'); wt(';');
    wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_fp", fc); wt(')'); wt('{');
    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    wt('}');
    /* int32 or float64 inline; anything else hands off to the cold helper.
       Assignment, not a declaration: the prologue already declared every v<n>. */
    wt_slot("v", nm); wt('=');
    wt('('); wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
    wt('?'); wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
    wt(':'); wt('('); wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
    wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
    wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(','); wt_slot("_fp", fc); wt(','); wt_id("deopt"); wt(')');
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
        wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_fs", fc); wt(')'); wt('{');
    } else {
        wk(JS_WK_IF); wt('('); wt_slot("_fs", fc); wt_op(JS_OP_NE);
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
        wt('{');
    }
    wt_slot("_fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_slot("_fs", fc); wt(')'); wt(';');
    wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_fp", fc); wt(')'); wt('{');
    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    wt('}');
    wk(JS_WK_IF); wt('('); wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(8); wt(')');
    wt('{'); wk(JS_WK_IF); wt('('); wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(0); wt(')');
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

/* Start buffering a loop body. Returns 0 when the stack is full, in which case
   the body is emitted straight through as before. */
static int loop_begin(int head_pos, int back_pos)
{
    WhaleLoop *l;

    if (whale_loop_depth >= WHALE_LOOP_MAX)
        return 0;
    l = &whale_loops[whale_loop_depth];
    l->len = 0;
    l->head_pos = head_pos;
    l->back_pos = back_pos;
    whale_out_tok = l->tok;
    whale_out_num = l->num;
    whale_out_len = &l->len;
    whale_loop_depth++;
    return 1;
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
    wt_slot("v", whale_vname[k]); wt('=');
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
static int whale_jit_gen(JSContext *ctx, const JSWhaleFuncInfo *info)
{
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
    int pos, i, sp, ret = 0, tail_mark;
    uint8_t *targets = NULL, *seen = NULL, *snap = NULL, *cur = NULL, *args_used = NULL;
    int *loop_back = NULL;      /* per position: the backward jump that closes it */
    uint8_t var_ref_used[4] = { 0, 0, 0, 0 };
    uint16_t *assign_cnt = NULL;
    uint8_t lo_c[WHALE_JIT_MAX_SLOTS];
    double lo_cv[WHALE_JIT_MAX_SLOTS];
    int unreachable = 0;
    int ret_is_bool = -1;
    int whale_objs = 0;   /* object slots handed out; each new object takes one */
    int whale_has_call = 0; /* a call clobbers registers, so no pinning then */
    int whale_fields = 0;   /* object-property cache pairs handed out */
    int whale_shape_stable = 0; /* object shapes cannot change after this point */
    int whale_def_atoms[WHALE_ATOM_MAX];
    int whale_put_atoms[WHALE_ATOM_MAX];
    int whale_def_atoms_n = 0;
    int whale_put_atoms_n = 0;
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
    assign_cnt = calloc(var_count > 0 ? var_count : 1, sizeof(uint16_t));
    whale_vexpr = calloc(stack_size, sizeof(WhaleVExpr));
    whale_vmat = calloc(1, stack_size);
    whale_lmem = calloc(var_count > 0 ? var_count : 1, sizeof(WhaleLocal));
    loop_back = malloc((len + 1) * sizeof(*loop_back));
    if (!targets || !seen || !snap || !cur || !args_used || !assign_cnt ||
        !whale_vexpr || !whale_vmat || !whale_lmem || !loop_back)
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
        } else if (op == OP_get_arg) {
            int a = (int)whale_get_u16(pc + pos + 1);
            if (a >= 0 && a < arg_count)
                args_used[a] = 1;
        } else if (op == OP_inc_loc || op == OP_dec_loc) {
            {   /* these write a local too, so it is not a constant */
                int a = (int)pc[pos + 1];
                if (a < var_count)
                    assign_cnt[a]++;
            }
        } else if (op >= OP_put_loc0 && op <= OP_put_loc3) {
            assign_cnt[op - OP_put_loc0]++;
        } else if (op == OP_put_loc8) {
            if ((int)pc[pos + 1] < var_count)
                assign_cnt[pc[pos + 1]]++;
        } else if (op == OP_put_loc) {
            int a = (int)whale_get_u16(pc + pos + 1);
            if (a < var_count)
                assign_cnt[a]++;
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
            if (whale_var_ref_feeds_call(pc, len, pos, sz))
                var_ref_used[op - OP_get_var_ref0] = 1;
        } else if (op == OP_get_var_ref) {
            int a = (int)whale_get_u16(pc + pos + 1);

            if (a >= 0 && a < 4 && whale_var_ref_feeds_call(pc, len, pos, sz))
                var_ref_used[a] = 1;
        }
        pos += sz;
    }

    {
        /* Count the back edges. A nested loop needs more live registers than a
           flat one, and pinning its counters is what made minmax spin for ever:
           with both induction variables pinned TinyCC has nowhere left to put a
           temporary. */
        int k, nloops = 0;

        for (k = 0; k < len; k++)
            if (loop_back[k])
                nloops++;
        whale_nloops = nloops;
    }

    /* A store to a name no literal defined would add a property, which changes
       the shape and invalidates every cached value pointer. Claim the shape is
       fixed only when that cannot happen, and only in a function with no calls -
       an object that cannot be passed anywhere cannot be reshaped from outside
       either. */
    whale_shape_stable = !whale_has_call;
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
        wk(JS_WK_INT); wt_id("idx"); wt(','); wk(JS_WK_DOUBLE); wt('*'); wt_id("out"); wt(')'); wt(';');    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefSetNum"); wt('(');
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
    wt_id("JSValue"); wt_id("_cref"); wt(';');
    wt_id("_cref"); wt('.'); wt_id("tag"); wt('='); wt_int(3); wt(';');
    for (i = 0; i < var_count; i++) {
        if (!whale_has_call && whale_nloops <= 1)
            wk(JS_WK_REGISTER);
        wk(JS_WK_DOUBLE); wt_slot("L", i); wt('='); wt_int(0); wt(';');
    }
    /* stack_size vars for the slots themselves plus WHALE_VSER_MAX spare names
       for the fresh names fold_materialize() hands out (see whale_vname) */
    for (i = 0; i < stack_size + WHALE_VSER_MAX; i++) {
        wk(JS_WK_DOUBLE); wt_slot("v", i); wt('='); wt_int(0); wt(';');
    }
    /* Per-property-access cache for object literals: the shape the property was
       resolved against and the address of its value. One pair per access site,
       and a site always touches the same property, so this is a compile-time
       constant. */
    for (i = 0; i < WHALE_FIELD_CACHE; i++) {
        wk(JS_WK_VOID); wt('*'); wt_slot("_fs", i); wt('='); wt_int(0); wt(';');
        /* JSValue *, not void *: the inlined read dereferences it */
        wt_id("JSValue"); wt('*'); wt_slot("_fp", i); wt('='); wt_int(0); wt(';');
    }
    /* Per-object-slot array cache: the backing store, its element count, and a
       flag. A store can move the backing store (the array grows), which clears
       the flag, so a store-heavy loop re-resolves and a scan does not. */
    for (i = 0; i < WHALE_OBJ_SLOTS; i++) {
        wt_id("JSValue"); wt('*'); wt_slot("_ap", i); wt('='); wt_int(0); wt(';');
        wk(JS_WK_DOUBLE); wt_slot("_an", i); wt('='); wt_int(0); wt(';');
        wk(JS_WK_INT); wt_slot("_ai", i); wt('='); wt_int(0); wt(';');
    }
    /* per-object-slot typed-array cache: base pointer, element count, and a flag
       so the pointer is fetched once per run rather than per element */
    for (i = 0; i < WHALE_OBJ_SLOTS; i++) {
        wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt_slot("_op", i); wt('='); wt_int(0); wt(';');
        wk(JS_WK_DOUBLE); wt_slot("_on", i); wt('='); wt_int(0); wt(';');
        wk(JS_WK_INT); wt_slot("_oi", i); wt('='); wt_int(0); wt(';');
    }
    for (i = 0; i < arg_count; i++) {
        if (args_used[i]) {
            wk(JS_WK_DOUBLE); wt_slot("A", i); wt('='); wt_id("An"); wt('['); wt_int(i); wt(']'); wt(';');
        }
    }
    fold_commit(0);     /* the whole implicit prologue is structural */

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
                    if ((sv == WHALE_T_I32 && whale_type_is_num(cv)) ||
                        (cv == WHALE_T_I32 && whale_type_is_num(sv))) {
                        snap[base + k] = WHALE_T_NUM;
                        continue;
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
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;
        case OP_push_i8:
            {
                int fm = fold_mark();

                wt_int((int)(int8_t)pc[pos + 1]);
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;
        case OP_push_i16:
            {
                int fm = fold_mark();

                wt_int((int)(int16_t)whale_get_u16(pc + pos + 1));
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;
        case OP_push_i32:
            {
                int fm = fold_mark();

                wt_int((int)(int32_t)whale_get_u32(pc + pos + 1));
                fold_set(sp, fm);
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
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
                        wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_ai", os); wt(')'); wt('{');
                        wt_slot("_ap", os); wt('='); wt_id("JS_WhaleArrPtr"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_slot("_an", os); wt(')'); wt(';');
                        wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_ap", os); wt(')'); wt('{');
                        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        wt_slot("_ai", os); wt('='); wt_int(1); wt(';');
                        wt('}');
                        /* the index, evaluated once and range-checked */
                        wt_slot("v", tk); wt('='); fold_inline(sp - 1); wt(';');
                        wk(JS_WK_IF); wt('('); wt_slot("v", tk); wt_op(JS_OP_LT); wt_int(0); wt_op(JS_OP_OR);
                        wt('('); wt_slot("v", tk); wt_op(JS_OP_GE); wt_slot("_an", os); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        /* int32 or float64 inline; anything else hands off */
                        wt_slot("v", nm); wt('=');
                        wt('('); wt_slot("_ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("v", tk); wt(']');
                        wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
                        wt('?'); wt_slot("_ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("v", tk); wt(']');
                        wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
                        wt(':'); wt('('); wt_slot("_ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("v", tk); wt(']');
                        wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
                        wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_slot("_ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("v", tk); wt(']');
                        wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
                        wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(',');
                        wt('&'); wt_slot("_ap", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("v", tk); wt(']'); wt(','); wt_id("deopt"); wt(')');
                        wt(';');
                        fold_commit(fm);
                        whale_vname[sp - 2] = nm;
                        whale_vmat[sp - 2] = 1;
                    }
                    cur[var_count + sp - 2] = WHALE_T_NUM;
                    sp--;
                    break;
                }
                if (!WHALE_IS_OBJ(ta) || !whale_type_is_num(tb))
                    goto out;
                os = WHALE_OBJ_SLOT(ta);
                nm = whale_vserial++;      /* result */
                tk = whale_vserial++;      /* the index, evaluated once */
                {
                    int fm = fold_mark();

                    /* Fetch the buffer once per run, then index it directly. A
                       call per element costs more than the interpreter. */
                    wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_oi", os); wt(')'); wt('{');
                    wt_slot("_op", os); wt('='); wt('('); wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt(')');
                    wt_id("JS_WhaleTAU8Ptr"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_slot("_on", os); wt(')'); wt(';');
                    wt_slot("_oi", os); wt('='); wt_int(1); wt(';'); wt('}');
                    wk(JS_WK_DOUBLE); wt_slot("_t", tk); wt('='); fold_inline(sp - 1); wt(';');
                    wk(JS_WK_IF); wt('('); wt('('); wt('!'); wt_slot("_op", os); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_LT); wt_int(0); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_GE); wt_slot("_on", os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    wt_slot("v", nm); wt('='); wt('('); wk(JS_WK_DOUBLE); wt(')');
                    wt_slot("_op", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("_t", tk); wt(']'); wt(';');
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

                if (whale_objs >= WHALE_OBJ_SLOTS)
                    goto out;
                os = whale_objs++;
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

        /* obj value -> obj */
        case OP_define_field:
            {
                int ta = cur[var_count + sp - 2];
                int tb = cur[var_count + sp - 1];
                int atom = (int)whale_get_u32(pc + pos + 1);

                if (!WHALE_IS_OBJ(ta) || !whale_type_is_num(tb))
                    goto out;
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(','); wt_int(atom); wt(',');
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

                if (!WHALE_IS_OBJ(ta) || !whale_type_is_num(tb))
                    goto out;
                if (whale_fields >= WHALE_FIELD_CACHE)
                    goto out;
                {
                    int fc = whale_fields++;
                    int fm;

                    fold_obj_field_resolve(WHALE_OBJ_SLOT(ta), atom, fc, whale_shape_stable);
                    fm = fold_mark();
                    /* the value is a double here, and the old one was a number too
                       (anything else deopted above) */
                    wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt('='); wt_int(8); wt(';');
                    wt_slot("_fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt('=');
                    fold_inline(sp - 1); wt(';');
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

                if (!WHALE_IS_OBJ(ta))
                    goto out;
                if (whale_fields >= WHALE_FIELD_CACHE)
                    goto out;
                fc = whale_fields++;
                nm = whale_vserial++;
                fold_obj_field_load(WHALE_OBJ_SLOT(ta), atom, fc, nm, whale_shape_stable);
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

                if (!WHALE_IS_OBJ(ta))
                    goto out;
                if (whale_fields >= WHALE_FIELD_CACHE)
                    goto out;
                fc = whale_fields++;
                nm = whale_vserial++;
                fold_obj_field_load(WHALE_OBJ_SLOT(ta), atom, fc, nm, whale_shape_stable);
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
                if (whale_objs >= WHALE_OBJ_SLOTS)
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

                if (!WHALE_IS_ARR(ta))
                    goto out;
                os = WHALE_OBJ_SLOT(ta);
                nm = whale_vserial++;
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_ai", os); wt(')'); wt('{');
                    wt_slot("_ap", os); wt('='); wt_id("JS_WhaleArrPtr"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_slot("_an", os); wt(')'); wt(';');
                    wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_ap", os); wt(')'); wt('{');
                    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    wt_slot("_ai", os); wt('='); wt_int(1); wt(';');
                    wt('}');
                    wt_slot("v", nm); wt('='); wt_slot("_an", os); wt(';');
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
                        wt_slot("_ai", os); wt('='); wt_int(0); wt(';');
                        wt_slot("v", tk); wt('='); fold_inline(sp - 2); wt(';');
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleArrSetNum"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_slot("v", tk); wt(',');
                        fold_inline(sp - 1); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    sp -= 3;
                    break;
                }
                if (!WHALE_IS_OBJ(ta) || !whale_type_is_num(tb) ||
                    !whale_type_is_num(tc))
                    goto out;
                os = WHALE_OBJ_SLOT(ta);
                tk = whale_vserial++;      /* the index, evaluated once */
                {
                    int fm = fold_mark();

                    wk(JS_WK_IF); wt('('); wt('!'); wt_slot("_oi", os); wt(')'); wt('{');
                    wt_slot("_op", os); wt('='); wt('('); wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt(')');
                    wt_id("JS_WhaleTAU8Ptr"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_slot("_on", os); wt(')'); wt(';');
                    wt_slot("_oi", os); wt('='); wt_int(1); wt(';'); wt('}');
                    wk(JS_WK_DOUBLE); wt_slot("_t", tk); wt('='); fold_inline(sp - 2); wt(';');
                    wk(JS_WK_IF); wt('('); wt('('); wt('!'); wt_slot("_op", os); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_LT); wt_int(0); wt(')'); wt_op(JS_OP_OR);
                    wt('('); wt_slot("_t", tk); wt_op(JS_OP_GE); wt_slot("_on", os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    wt_slot("_op", os); wt('['); wt('('); wk(JS_WK_INT); wt(')'); wt_slot("_t", tk); wt(']'); wt('=');
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
                if (WHALE_IS_OBJ(t) || WHALE_IS_ARR(t)) {
                    /* An object is its slot number, which the type already
                       carries - nothing to emit, the value is the same in every
                       copy. Only the value path needs folding. */
                    cur[var_count + sp] = (uint8_t)t;
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
                if (WHALE_IS_OBJ(t) || WHALE_IS_ARR(t)) {
                    /* Same reasoning as get_loc: the value *is* the slot number,
                       so this store is a type change and nothing else - no
                       deferred store and no fold slot. It can put a different
                       array in the slot, so drop any cached buffer pointer. */
                    int fm = fold_mark();

                    wt_slot("_oi", WHALE_OBJ_SLOT(t)); wt('='); wt_int(0); wt(';');
                    fold_commit(fm);
                    local_forget(idx);
                    cur[idx] = (uint8_t)t;
                    lo_c[idx] = 0;
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
                    wt_id("var_refs"); wt(','); wt_int(idx); wt(','); wt('&'); wt_slot("v", nm); wt(')');
                    wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;
                    cur[var_count + sp] = WHALE_T_NUM;
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
                if (JS_WhaleGlobalIsU8Ctor(ctx, atom)) {
                    /* `new Uint8Array(...)` needs the constructor itself kept.
                       Judged here, while emitting, so the slot's identity is a
                       single compile-time fact - see the WHALE_OBJ_T note. */
                    int slot = whale_objs++;
                    int nm;

                    if (slot >= WHALE_OBJ_SLOTS)
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
                    wt_id("JS_WhaleGetGlobalNum"); wt('('); wt_id("ctx"); wt(','); wt_int(atom); wt(','); wt('&'); wt_slot("v", nm); wt(')');
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

                if (func_slot < 0 || cur[var_count + func_slot] != WHALE_T_SELF)
                    goto out;
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
                    wt_slot("v", nm); wt('='); wt_id("_r"); wt(';'); wt('}');
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
                if (opid < 0 || !whale_type_is_num(ta) || !whale_type_is_num(tb))
                    goto out;
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
                int fm = fold_mark();

                fold_mod(sp - 2, sp - 1);
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
                fold_toint32(sp - 2, cur[var_count + sp - 2] == WHALE_T_I32);
                wt(op == OP_and ? '&' : op == OP_or ? '|' : '^');
                fold_toint32(sp - 1, cur[var_count + sp - 1] == WHALE_T_I32);
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
                fold_toint32(sp - 2, cur[var_count + sp - 2] == WHALE_T_I32);
                /* one token, not two '<' - there is no lexer to join them */
                wt_op(op == OP_shl ? JS_OP_SHL : JS_OP_SAR);
                wt('('); fold_toint32(sp - 1, cur[var_count + sp - 1] == WHALE_T_I32); wt('&'); wt_int(31); wt(')');
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

                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('~'); fold_toint32(sp - 1, cur[var_count + sp - 1] == WHALE_T_I32);
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
                    /* The slot holds the reference; hand it to the caller
                       through ret_obj. Mixing it with the numeric return path
                       is refused - a function with two return shapes is not to
                       be specialised. */
                    if (ret_is_bool != -1)
                        goto out;
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
        wk(JS_WK_IF); wt('('); wt('!'); wt_id("whale_jit_unbox_num"); wt('('); wt_id("ctx"); wt(','); wt_id("args"); wt('['); wt_int(i); wt(']'); wt(','); wt('&'); wt_id("An"); wt('['); wt_int(i); wt(']'); wt(')'); wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';'); wt('}');
    }
    /* closure slots we rely on must be this very function (once per entry;
       the closure cannot change during the call, so recursion need not re-check) */
    for (i = 0; i < 4; i++) {
        if (var_ref_used[i]) {
            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleVarRefIsSelf"); wt('('); wt_id("ctx"); wt(','); wt_id("whale_var_refs"); wt(','); wt_int(i); wt(','); wt_id("whale_self"); wt(')'); wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';'); wt('}');
        }
    }
    wt_id("_r"); wt('='); wt_id(whale_core_name); wt('('); wt_id("ctx"); wt(','); wt_id("args"); wt(','); wt_id("An"); wt(','); wt_id("deopt"); wt(','); wt('&'); wt_id("_ret"); wt(','); wt_id("whale_var_refs"); wt(')'); wt(';');
    wk(JS_WK_IF); wt('('); wt('*'); wt_id("deopt"); wt(')'); wk(JS_WK_RETURN); wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(','); wt_int(0); wt(')'); wt(';');
    /* an array or object result came back through ret_obj instead */
    wk(JS_WK_IF); wt('('); wt_id("_ret"); wt('.'); wt_id("tag"); wt_op(JS_OP_NE); wt_int(3); wt(')');
    wk(JS_WK_RETURN); wt_id("_ret"); wt(';');
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
        whale_jit_state = s;
    }
    s = whale_jit_state;

    /* fresh names per compile: the code accumulates in this one state */
    snprintf(whale_core_name, sizeof(whale_core_name), "whale_jit_core_%u", ++whale_jit_seq);
    snprintf(whale_entry_name, sizeof(whale_entry_name), "whale_jit_fn_%u", whale_jit_seq);

    /* Drive TinyCC's parser with the tokens the generator pushed: no source
       text is parsed and no header is read. */
    (void)src;
    if (!JS_WhaleCompileTokens(s, whale_gen_cb, NULL))
        goto fail;
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