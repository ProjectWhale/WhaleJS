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

#ifndef WHALE_JIT_TCC_DIR
#define WHALE_JIT_TCC_DIR "thirdparty/tcc-runtime"
#endif
#ifndef WHALE_JIT_SRC_DIR
#define WHALE_JIT_SRC_DIR "src"
#endif

#define WHALE_JIT_SRC_MAX 65536
#define WHALE_JIT_MAX_SLOTS 64

/* type lattice for the specialization pass */
enum {
    WHALE_T_UNKNOWN = 0,
    WHALE_T_NUM = 1,  /* Number: held as double while specialized */
    WHALE_T_BOOL = 2, /* true / false, usable as 1 / 0 */
    WHALE_T_SELF = 3, /* this function, usable only as a call target */
};

static char whale_jit_src[WHALE_JIT_SRC_MAX];
static size_t whale_jit_src_len;

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
static void whale_emit(const char *fmt, ...)
{
    va_list ap;
    int n;

    if (whale_jit_src_len + 1 >= WHALE_JIT_SRC_MAX)
        return;
    va_start(ap, fmt);
    n = vsnprintf(whale_jit_src + whale_jit_src_len,
                  WHALE_JIT_SRC_MAX - whale_jit_src_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        whale_jit_src_len += (size_t)n;
}

/* ---- helpers the generated code calls back into ---- */

/* Is closure slot idx of the frame that is currently running equal to `self`?
   Only true for a direct self-recursive reference, which is the single call
   shape we compile. Borrowed reference: no dup/free needed. */

/* Self-recursive call: go back through the engine so the callee gets a proper
   frame. The callee is this same function object, so it re-enters this JIT
   entry rather than the interpreter. */


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

static const char *whale_jit_preamble =
    "#include \"quickjs.h\"\n"
    "extern int JS_WhaleVarRefIsSelf(JSContext *ctx, void **var_refs, int idx, JSValue self);\n"
    "extern JSValue whale_jit_box_num(JSContext *ctx, double d);\n"
    "extern int JS_WhaleCheckStack(JSContext *ctx);\n"
    "extern int whale_jit_unbox_num(JSContext *ctx, JSValue v, double *out);\n";

static int whale_type_is_num(int t)
{
    return t == WHALE_T_NUM || t == WHALE_T_BOOL;
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
    int pos, i, sp, ret = 0;
    uint8_t *targets = NULL, *seen = NULL, *snap = NULL, *cur = NULL, *args_used = NULL;
    uint8_t var_ref_used[4] = { 0, 0, 0, 0 };
    uint16_t *assign_cnt = NULL;
    uint8_t lo_c[WHALE_JIT_MAX_SLOTS];
    double lo_cv[WHALE_JIT_MAX_SLOTS];
    int unreachable = 0;
    int ret_is_bool = -1;

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
    if (!targets || !seen || !snap || !cur || !args_used || !assign_cnt)
        goto out;
    memset(lo_c, 0, sizeof(lo_c));
    memset(lo_cv, 0, sizeof(lo_cv));

    /* pass 1: jump targets + which arguments are read */
    for (pos = 0; pos < len; ) {
        int op = pc[pos];
        int sz = whale_op_size_of(op);

        if (sz <= 0 || pos + sz > len)
            goto out;
        if (op == OP_goto8 || op == OP_if_false8 || op == OP_if_true8) {
            targets[pos + 1 + (int8_t)pc[pos + 1]] = 1;
        } else if (op == OP_goto || op == OP_if_false || op == OP_if_true) {
            targets[pos + 1 + (int32_t)whale_get_u32(pc + pos + 1)] = 1;
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
        } else if (op >= OP_get_var_ref0 && op < OP_get_var_ref0 + 4) {
            var_ref_used[op - OP_get_var_ref0] = 1;
        } else if (op == OP_get_var_ref) {
            int a = (int)whale_get_u16(pc + pos + 1);
            if (a >= 0 && a < 4)
                var_ref_used[a] = 1;
        }
        pos += sz;
    }

    /* pass 2: emit tokens. Nothing here builds text - the generator pushes
       tokens into TinyCC's parser directly, so there is no lexer and no
       keyword table, and keywords are compile-time constants. */

    /* preamble: types only (JSValue is 8 bytes by value, ABI-identical to a
       quickjs JSValue), the one extern constant, and the entry points we call.
       No #include, so the compile needs no file on disk. */
    wk(JS_WK_TYPEDEF); wk(JS_WK_UNSIGNED); wk(JS_WK_LONG); wk(JS_WK_LONG); wt_id("JSValue"); wt(';');
    wk(JS_WK_TYPEDEF); wk(JS_WK_VOID); wt_id("JSContext"); wt(';');
    wt(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_EXCEPTION"); wt(';');
    wt(JS_WK_EXTERN); wt_id("JSContext"); wt('*'); wt_id("JS_WhaleVarRefIsSelf"); wt('(');
        wt_id("JSContext"); wt('*'); wt_id("ctx"); wt(','); wt_id("void"); wt('*'); wt('*');
        wt_id("var_refs"); wt(','); wt(JS_WK_INT); wt_id("idx"); wt(',');
        wt_id("JSValue"); wt_id("self"); wt(')'); wt(';');
    wt(JS_WK_EXTERN); wt_id("JSValue"); wt_id("whale_jit_box_num"); wt('(');
        wt_id("JSContext"); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_DOUBLE); wt_id("d"); wt(')'); wt(';');
    wt(JS_WK_EXTERN); wt(JS_WK_INT); wt_id("JS_WhaleCheckStack"); wt('(');
        wt_id("JSContext"); wt('*'); wt_id("ctx"); wt(')'); wt(';');
    wt(JS_WK_EXTERN); wt(JS_WK_INT); wt_id("whale_jit_unbox_num"); wt('(');
        wt_id("JSContext"); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt_id("v"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("dst"); wt(')'); wt(';');
    wt(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_NewFloat64"); wt('(');
        wt_id("JSContext"); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_DOUBLE); wt_id("d"); wt(')'); wt(';');
    wt(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_NewBool"); wt('(');
        wt_id("JSContext"); wt('*'); wt_id("ctx"); wt(','); wt(JS_WK_INT); wt_id("b"); wt(')'); wt(';');

    /* whale_jit_core works purely in doubles: every number is unboxed at the
       boundary. Self-recursion calls it directly, so the recursive fast path
       costs no boxing at all - only the engine entry does that. */
    wk(JS_WK_STATIC); wk(JS_WK_DOUBLE); wt_id("whale_jit_core"); wt('(');
        wt_id("JSContext"); wt('*'); wt_id("ctx"); wt(',');
        wt_id("JSValue"); wt('*'); wt_id("args"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("An"); wt(',');
        wt(JS_WK_INT); wt('*'); wt_id("deopt"); wt(')'); wt('{');
    for (i = 0; i < var_count; i++) {
        wk(JS_WK_DOUBLE); wt_slot("L", i); wt('='); wt_int(0); wt(';');
    }
    for (i = 0; i < stack_size; i++) {
        wk(JS_WK_DOUBLE); wt_slot("v", i); wt('='); wt_int(0); wt(';');
    }
    for (i = 0; i < arg_count; i++) {
        if (args_used[i]) {
            wk(JS_WK_DOUBLE); wt_slot("A", i); wt('='); wt_id("An"); wt('['); wt_int(i); wt(']'); wt(';');
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

        /* dead code after an unconditional return/goto is not emitted */
        if (unreachable && !targets[pos]) {
            pos += sz;
            continue;
        }
        if (targets[pos])
            unreachable = 0;

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
            wt_slot("L", pos); wt(':');
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

                if (is_cmp) {
                    if (dst >= 0) {
                        wt_slot("L", dst); wt('='); wt('('); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0); wt(';');
                        cur[dst] = WHALE_T_BOOL;
                    } else {
                        wt_slot("v", sp); wt('='); wt('('); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0); wt(';');
                        cur[var_count + sp] = WHALE_T_BOOL;
                        sp += 1;
                    }
                } else {
                    if (dst >= 0) {
                        wt_slot("L", dst); wt('='); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(';');
                        cur[dst] = WHALE_T_NUM;
                    } else {
                        wt_slot("v", sp); wt('='); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(';');
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
                wt_slot("L", d2); wt('='); whale_load_emit(k1, i1, cv1); wt(';');
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

        switch (op) {
        case OP_push_minus1:
        case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
        case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
            wt_slot("v", sp); wt('='); wt_int(op == OP_push_minus1 ? -1 : op - OP_push_0); wt(';');
            cur[var_count + sp] = WHALE_T_NUM;
            sp++;
            break;
        case OP_push_i8:
            wt_slot("v", sp); wt('='); wt_int((int)(int8_t)pc[pos + 1]); wt(';');
            cur[var_count + sp] = WHALE_T_NUM;
            sp++;
            break;
        case OP_push_i16:
            wt_slot("v", sp); wt('='); wt_int((int)(int16_t)whale_get_u16(pc + pos + 1)); wt(';');
            cur[var_count + sp] = WHALE_T_NUM;
            sp++;
            break;
        case OP_push_i32:
            wt_slot("v", sp); wt('='); wt_int((int)(int32_t)whale_get_u32(pc + pos + 1)); wt(';');
            cur[var_count + sp] = WHALE_T_NUM;
            sp++;
            break;
        case OP_push_const:
        case OP_push_const8:
            {
                int idx = (op == OP_push_const8) ? (int)pc[pos + 1] : (int)whale_get_u32(pc + pos + 1);
                JSValue c;
                int ct;

                if (idx < 0 || idx >= info->cpool_count)
                    goto out;
                c = info->cpool[idx];
                ct = JS_VALUE_GET_TAG(c);
                if (ct == JS_TAG_INT) {
                    wt_slot("v", sp); wt('='); wt_int((int)JS_VALUE_GET_INT(c)); wt(';');
                    cur[var_count + sp] = WHALE_T_NUM;
                    sp++;
                } else if (JS_TAG_IS_FLOAT64(ct)) {
                    wt_slot("v", sp); wt('='); wt_num(JS_VALUE_GET_FLOAT64(c)); wt(';');
                    cur[var_count + sp] = WHALE_T_NUM;
                    sp++;
                } else {
                    goto out; /* strings / bigints / objects: not our shape */
                }
            }
            break;
        case OP_push_true:
            wt_slot("v", sp); wt('='); wt_int(1); wt(';');
            cur[var_count + sp] = WHALE_T_BOOL;
            sp++;
            break;
        case OP_push_false:
            wt_slot("v", sp); wt('='); wt_int(0); wt(';');
            cur[var_count + sp] = WHALE_T_BOOL;
            sp++;
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
                wt_slot("v", sp); wt('='); wt_slot("L", idx); wt(';');
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
                t = cur[var_count + sp - 1];
                if (t == WHALE_T_UNKNOWN || idx < 0 || idx >= var_count)
                    goto out;
                wt_slot("L", idx); wt('='); wt_slot("v", sp - 1); wt(';');
                cur[idx] = (uint8_t)t;
                lo_c[idx] = 0;
                sp--;
            }
            break;
        case OP_get_loc0_loc1:
            if (var_count < 2 || cur[0] == WHALE_T_UNKNOWN || cur[1] == WHALE_T_UNKNOWN)
                goto out;
            wt_slot("v", sp); wt('='); wt_slot("L", 0); wt(';'); wt_slot("v", sp + 1); wt('='); wt_slot("L", 1); wt(';');
            cur[var_count + sp] = cur[0];
            cur[var_count + sp + 1] = cur[1];
            sp += 2;
            break;
        case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
        case OP_get_arg:
            {
                int idx = (op == OP_get_arg) ? (int)whale_get_u16(pc + pos + 1) : op - OP_get_arg0;

                if (idx < 0 || idx >= arg_count)
                    goto out;
                wt_slot("v", sp); wt('='); wt_slot("A", idx); wt(';');
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

                /* the closure check was hoisted to the entry */
                (void)idx;
                cur[var_count + sp] = WHALE_T_SELF;
                sp++;
            }
            break;

        /* self-recursive call */
        case OP_call0: case OP_call1: case OP_call2: case OP_call3:
        case OP_call:
            {
                int argc = (op == OP_call) ? (int)whale_get_u16(pc + pos + 1) : op - OP_call0;
                int func_slot = sp - 1 - argc;
                int k;

                if (func_slot < 0 || cur[var_count + func_slot] != WHALE_T_SELF)
                    goto out;
                for (k = 0; k < argc; k++) {
                    if (!whale_type_is_num(cur[var_count + sp - argc + k]))
                        goto out;
                }
                whale_emit("    { double _an[%d]; double _r = 0; int _dd = 0;\n", argc > 0 ? argc : 1);
                for (k = 0; k < argc; k++)
                    whale_emit("      _an[%d] = v%d;\n", k, sp - argc + k);
                whale_emit("      if (!JS_WhaleCheckStack(ctx)) { *deopt = 1; return 0; }\n");
                whale_emit("      _r = whale_jit_core(ctx, args, _an, &_dd);\n");
                whale_emit("      if (_dd) { *deopt = 1; return 0; }\n");
                whale_emit("      v%d = _r; }\n", func_slot);
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
                    wt_slot("v", sp - 2); wt('='); wt('('); wt_slot("v", sp - 2); wt_op(opid); wt_slot("v", sp - 1); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0); wt(';');
                    cur[var_count + sp - 2] = WHALE_T_BOOL;
                } else {
                    wt_slot("v", sp - 2); wt('='); wt_slot("v", sp - 2); wt_op(opid); wt_slot("v", sp - 1); wt(';');
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
            wt_slot("v", sp - 2); wt('='); wt_id("fmod"); wt('('); wt_slot("v", sp - 2); wt(','); wt_slot("v", sp - 1); wt(')'); wt(';');
            cur[var_count + sp - 2] = WHALE_T_NUM;
            sp--;
            break;
        case OP_neg:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            wt_slot("v", sp - 1); wt('='); wt('-'); wt_slot("v", sp - 1); wt(';');
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        case OP_plus:
            /* +x on a number is the identity */
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        case OP_not:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            /* !x is true for 0 and for NaN */
            whale_emit("    v%d = ((v%d != v%d) || (v%d == 0)) ? 1 : 0;\n", sp - 1, sp - 1, sp - 1, sp - 1);
            cur[var_count + sp - 1] = WHALE_T_BOOL;
            break;
        case OP_inc: case OP_dec:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                goto out;
            wt_slot("v", sp - 1); wt('='); wt_slot("v", sp - 1); wt(op == OP_inc ? '+' : '-'); wt_int(1); wt(';');
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        case OP_inc_loc:
            {
                int idx = (int)pc[pos + 1];
                if (idx < 0 || idx >= var_count || !whale_type_is_num(cur[idx]))
                    goto out;
                wt_slot("L", idx); wt('='); wt_slot("L", idx); wt('+'); wt_int(1); wt(';');
                cur[idx] = WHALE_T_NUM;
                lo_c[idx] = 0;
            }
            break;
        case OP_drop:
            if (sp <= 0)
                goto out;
            sp--;
            break;
        case OP_dup:
            if (sp <= 0)
                goto out;
            wt_slot("v", sp); wt('='); wt_slot("v", sp - 1); wt(';');
            cur[var_count + sp] = cur[var_count + sp - 1];
            sp++;
            break;

        case OP_if_false8: case OP_if_true8: case OP_if_false: case OP_if_true:
            {
                int is8 = (op == OP_if_false8 || op == OP_if_true8);
                int is_true = (op == OP_if_true8 || op == OP_if_true);
                int tgt = is8 ? (pos + 1 + (int8_t)pc[pos + 1])
                              : (pos + 1 + (int32_t)whale_get_u32(pc + pos + 1));

                t = cur[var_count + sp - 1];
                if (!whale_type_is_num(t))
                    goto out;
                whale_emit("    if (v%d %s 0) goto L%d;\n", sp - 1, is_true ? "!=" : "==", tgt);
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

                whale_emit("    goto L%d;\n", tgt);
                unreachable = 1;
                if ((tgt >= 0) && (tgt <= len) && (!seen[tgt])) {
                    cur[state_len - 1] = (uint8_t)sp;
                    seen[tgt] = 1;
                    memcpy(snap + (size_t)tgt * state_len, cur, state_len);
                }
            }
            break;
        case OP_return:
            t = cur[var_count + sp - 1];
            if (t != WHALE_T_BOOL && t != WHALE_T_NUM)
                goto out;
            if (ret_is_bool == -1)
                ret_is_bool = (t == WHALE_T_BOOL);
            else if (ret_is_bool != (t == WHALE_T_BOOL))
                goto out; /* mixed number/boolean returns: not our shape */
            whale_emit("    return v%d;\n", sp - 1);
            unreachable = 1;
            break;
        default:
            goto out; /* anything else: keep interpreting */
        }
        pos += sz;
    }

    whale_emit("    *deopt = 1;\n    return 0;\n");
    whale_emit("}\n");
    /* engine entry: unbox the arguments once, run core, box the result */
    whale_emit("JSValue whale_jit_fn(JSContext *ctx, JSValue *args, JSValue *var_buf, int *deopt, JSValue whale_self, void *whale_entry, void *whale_var_refs)\n{\n");
    whale_emit("    double An[%d]; double _r;\n", arg_count > 0 ? arg_count : 1);
    whale_emit("    (void)ctx; (void)var_buf; (void)args; (void)deopt; (void)whale_self; (void)whale_entry; (void)whale_var_refs;\n");
    whale_emit("    if (!JS_WhaleCheckStack(ctx)) return JS_EXCEPTION;\n");
    for (i = 0; i < arg_count; i++) {
        if (!args_used[i])
            continue;
        whale_emit("    if (!whale_jit_unbox_num(ctx, args[%d], &An[%d])) { *deopt = 1; return JS_EXCEPTION; }\n", i, i);
    }
    /* closure slots we rely on must be this very function (once per entry;
       the closure cannot change during the call, so recursion need not re-check) */
    for (i = 0; i < 4; i++) {
        if (var_ref_used[i])
            whale_emit("    if (!JS_WhaleVarRefIsSelf(ctx, whale_var_refs, %d, whale_self)) { *deopt = 1; return JS_EXCEPTION; }\n", i);
    }
    whale_emit("    _r = whale_jit_core(ctx, args, An, deopt);\n");
    whale_emit("    if (*deopt) return JS_EXCEPTION;\n");
    if (ret_is_bool == 1)
        whale_emit("    return JS_NewBool(ctx, _r != 0);\n");
    else
        whale_emit("    return whale_jit_box_num(ctx, _r);\n");
    whale_emit("}\n");
    ret = 1; /* every opcode lowered successfully */
  out:
    if (!ret && getenv("WHALE_JIT_DUMP"))
        fprintf(stderr, "whale jit: gen gave up at pos=%d op=%d\n", pos, (pos < len) ? pc[pos] : -1);
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
    if (assign_cnt)
        free(assign_cnt);
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

static int whale_tcc_compile(JSContext *ctx, const char *src, void **entry_out, void **handle_out)
{
    TCCState *s;
    void *fn;

    if (getenv("WHALE_JIT_DUMP"))
        fprintf(stderr, "whale jit paths: src=%s rt=%s\n", WHALE_JIT_SRC_DIR, WHALE_JIT_TCC_DIR);

    s = tcc_new();
    if (!s)
        return 0;

    tcc_set_lib_path(s, WHALE_JIT_TCC_DIR);
    tcc_add_include_path(s, WHALE_JIT_TCC_DIR "/include");
    tcc_add_include_path(s, WHALE_JIT_SRC_DIR);
    tcc_add_library_path(s, WHALE_JIT_TCC_DIR "/lib");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_add_symbol(s, "JS_WhaleVarRefIsSelf", (void *)JS_WhaleVarRefIsSelf);
    tcc_add_symbol(s, "JS_WhaleCheckStack", (void *)JS_WhaleCheckStack);
    tcc_add_symbol(s, "whale_jit_box_num", (void *)whale_jit_box_num);
    tcc_add_symbol(s, "whale_jit_unbox_num", (void *)whale_jit_unbox_num);
    tcc_add_symbol(s, "JS_NewFloat64", (void *)JS_NewFloat64);

    if (tcc_compile_string(s, src) < 0)
        goto fail;
    if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0)
        goto fail;
    fn = tcc_get_symbol(s, "whale_jit_fn");
    if (!fn)
        goto fail;

    *entry_out = fn;
    *handle_out = s;
    return 1;
  fail:
    tcc_delete(s);
    return 0;
}

static void whale_tcc_release(void *handle)
{
    if (handle)
        tcc_delete(handle);
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
    if (!whale_jit_gen(ctx, info))
        return 0;

    if (getenv("WHALE_JIT_DUMP")) {
        fprintf(stderr, "--- whale jit source ---\n%s--- end ---\n", whale_jit_src);
        fprintf(stderr, "whale tcc internal probe: vstack cap=%d\n", JS_WhaleTccProbe());

    }

    if (!whale_jit_backend->compile(ctx, whale_jit_src, &entry, &handle))
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