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
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdarg.h>

#include "quickjs.h"
#include "whale_dbg.h"
#include "whale_jit.h"
#include "whalejs.h"

#include "libtcc.h"

/* implemented inside the TinyCC translation unit (src/tcc/whale_tcc_iface.c) */
int JS_WhaleTccProbe(void);
void JS_WhaleTccClearRuntimeMain(struct TCCState *s);
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
    WHALE_T_VRFN = 11, /* a closure slot read as a call target, resolved at
                          compile time to the function it held then. The call is
                          expanded inline behind an identity guard: if the slot
                          no longer holds that function, the guard deopts. The
                          byte carries the var_ref index, not a slot number. */
    WHALE_T_IBIT = 12, /* a bare int expression (no `(double)` wrapper): produced
                          by a bitwise op whose operands were all int-clean, or
                          by an int-region mirror read. Consuming it is zero
                          conversion (it already is an int); anything needing a
                          double (arithmetic, compare promotion aside) wraps it.
                          Merges down to NUM like I32 - it is a hint, not a value. */
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
/* a closure slot resolved to a function at compile time: the byte carries the
   var_ref index (0..3), not a slot number */
#define WHALE_VRFN_T(idx)  ((uint8_t)(WHALE_T_VRFN | (((idx) + 1) << 4)))
#define WHALE_IS_VRFN(t)   (((t) & 0x0f) == WHALE_T_VRFN)
#define WHALE_VRFN_IDX(t)  ((int)((t) >> 4) - 1)

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

/* Every type that carries an object slot in its high nibble, structs included.

   WHALE_IS_SLOT above leaves structs out on purpose: a frame-column struct keeps
   its values in the columns, so a site that would treat the slot as a parked
   JSValue must refuse it. But the question "which slot is this object in" has the
   same answer for all four, and so does "may this leave the function" and "can two
   edges meet here". Asking those with the three-of-four predicate is how a run of
   separate bugs happened, each one a site that had forgotten about structs:

     - `return` enumerated ARR/OBJ/VAL by hand instead of using a predicate, so a
       function returning a laid-out object was refused outright;
     - put_field and get_field tested `!WHALE_IS_SLOT(ta)` before their struct arm,
       so every read and write on an unboxed object went to the engine instead;
     - put_loc and set_loc each handled a struct without registering its object slot
       in loc_slot[], so the entry edge and the back edge of a loop disagreed about
       which slot held the variable.

   Use this predicate for those questions. Use WHALE_IS_SLOT where the point is
   "this is a JSValue in a slot" - that is what it is for. */
#define WHALE_IS_OBJLIKE(t)  (WHALE_IS_SLOT(t) || WHALE_IS_STRUCT(t))

/* ------------------------------------------------------------------------
 * Token emitters.
 *
 * The generator pushes tokens straight into TinyCC's parser - there is no text
 * stage and therefore no lexer, no keyword table and no literal scanner. The
 * keywords are compile-time constants here (TOK_DOUBLE, TOK_RETURN, ...), so
 * nothing can be mistaken for an identifier or collide with a TinyCC keyword.
 * ---------------------------------------------------------------------- */
/* wk = keyword (JS_WK_*), wt = punctuation / single-char operator */
static void wsrc(const char *s);   /* the generated-source mirror, defined below */
static void whale_comma_close(void);   /* comma-fused stores, defined below */
static int whale_comma_open;   /* comma sequence state, defined below */
static int *whale_comma_out;   /* output buffer the open statement lives in */
static int whale_comma_base;   /* output length the open statement started at */
static int whale_comma_len;    /* token length of the open statement so far */

static void wk(int w)
{
    /* Mirror the keyword as well. JS_WhaleTokKw pushes a TinyCC token straight
       through, so the WHALE_JIT_SRC dump used to be missing every keyword - a
       `return` read as an absent `return`, and `if (...) { ... }` read as a bare
       block. Both sent this session's debugging after things that were not there.
       The order matches JS_WhaleTokKw's table. */
    static const char *const kwname[] = {
        "int", "char", "void", "long", "unsigned", "float",
        "double", "static", "extern", "typedef", "struct", "return",
        "if", "else", "while", "for", "do", "break", "continue",
        "goto", "switch", "case", "default", "sizeof", "register",
        "union", "enum", "short",
    };

    /* `if` and `goto` always start a statement in our output (no expression-
       level ifs): an open comma sequence ends here. This covers every branch
       emission (compare-into-branch, head test, back edge) without touching
       each site - and expression keywords (types, return is handled at its
       own site) never trip it. */
    if (w == JS_WK_IF || w == JS_WK_GOTO)
        whale_comma_close();
    if (w >= 0 && w < (int)(sizeof(kwname) / sizeof(kwname[0]))) {
        wsrc(kwname[w]);
        wsrc(" ");
    }
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
    /* The generated-source mirror lives in whale_next_token() now, where the token
       stream is consumed in its final order. Mirroring here recorded an expression
       while it was still pending in the fold sink, so the text was out of order and
       folded expressions were absent. This stays as the one call site it was, kept
       so the emit helpers do not all have to change. */
    (void)s;
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

/* A slot of the generated function's scratch state. Value temporaries live
   in a bare `_wsv[]` array (never zero-initialised: every slot is assigned
   before it is read, so initialising it is pure per-call tax - fib paid ~3KB
   of it 240M times). Everything else (field caches, call slots, TAU8
   caches, presence flags) lives in the zero-initialised `_wsc` block, whose
   zero state is load-bearing (fv[] == 0 means "uncached", fver starts at 1
   to differ from it). */
static void wt_arr(const char *field, int n)
{
    if (field[0] == 'v' && field[1] == '\0') {
        wt_id("_wsv"); wt('['); wt_int(n); wt(']');
        return;
    }
    wt_id("_wsc"); wt('.'); wt_id(field); wt('['); wt_int(n); wt(']');
}

/* ELS: the hottest materialised slots skip the state block and live in
   pinned `register double` locals (`_wv0..7`, declared in the prologue).
   whale_vname values >= WHALE_VPIN_BASE are pin slots (index minus base),
   smaller ones are _ws.v[] indices as before. Only fold_materialize() hands
   out pin slots (never address-taken: field helpers take &v[nm] of their own
   fresh names, never of a fold slot), and only inside loops (a materialised
   value there is recomputed per trip and re-read, so the register pays). */
#define WHALE_VPIN_BASE 100
#define WHALE_VPIN_MAX 8
static int whale_vpin_next;   /* pin slots handed out, reset per compile */
static void wt_vname(int nm)
{
    if (nm >= WHALE_VPIN_BASE)
        wt_slot("_wv", nm - WHALE_VPIN_BASE);
    else
        wt_arr("v", nm);
}

static void wt_op(int o)
{
    /* Mirror the operator as well. wt() mirrors single characters, but every
       multi-character operator comes through here and goes straight to TinyCC, so
       the generated-source dump used to show `a b` where the code was `a + b`.
       That is what made a correct `st.a = st.a + st.e` read as an assignment of
       nothing, and sent this session after a bug that was not there. The order
       matches JS_WhaleTokOp's table. */
    static const char *const opname[] = {
        "+", "-", "*", "/", "<", "<=", ">", ">=", "==", "!=", "||", "&&",
        "<<", ">>", ">>>", "->", ".",
    };

    if (o >= 0 && o < (int)(sizeof(opname) / sizeof(opname[0]))) {
        wsrc(opname[o]);
        wsrc(" ");
    }
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
/* WhaleJS: dump the finished code section so it can be disassembled. Defined in
   tccgen.c, which is the only place text_section is in scope. */
void JS_WhaleDumpCode(FILE *f);
/* ELS pin map scope: one JIT compilation (core + wrapper share it). Defined in
   tccgen.c next to the mechanism. */
void JS_WhalePinEnter(void);
void JS_WhalePinLeave(void);

static char whale_core_name[40];
static char whale_entry_name[40];


/* quickjs.h declares these as static inline, so they have no external linkage
   and nothing outside this translation unit can resolve them - which a shared
   object loaded at run time must. Generated code calls them, so they get
   wrappers that do have linkage, and the emitted prototype names the wrapper.
   Registering the wrapper in whale_host_syms is what keeps TinyCC resolving the
   same name, because both backends read that one table. */
JSValue JS_WhaleNewFloat64(JSContext *ctx, double d)
{
    return JS_NewFloat64(ctx, d);
}

JSValue JS_WhaleNewBool(JSContext *ctx, int b)
{
    return JS_NewBool(ctx, b);
}
JSValue whale_jit_box_num(JSContext *ctx, double d)
{
    if (d >= -2147483648.0 && d <= 2147483647.0) {
        int i = (int)d;

        if ((double)i == d)
            return JS_MKVAL(JS_TAG_INT, i);
    }
    return JS_NewFloat64(ctx, d);
}

/* returns 1 and stores the value when v is a number */
int whale_jit_unbox_num(JSContext *ctx, JSValue v, double *out)
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
    /* `new F(...)` takes its callee the same way a call does: the slot read just
       before it is a function, not a number. Leaving this out typed `Error` as a
       number, and `throw new Error(m)` then refused the whole function. */
    case OP_call_constructor:
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

/* Pure-numeric opcodes: the generator lowers them to C operators and plain
   variable traffic without calling any helper. Anything else (calls, field /
   array / global access, conversions that need ToInt32/fmod, throws, catches)
   may emit a helper call inside a loop body - and a pinned local spanning such
   a call pays spill+reload per call for zero net gain. The ELS local-pin
   decision consults this per loop body; unlisted opcodes conservatively count
   as "has a call" (safe direction: less pinning, never wrong code). */
static int whale_is_pure_num_op(int op)
{
    switch (op) {
    case OP_push_minus1:
    case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
    case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
    case OP_push_i8: case OP_push_i16: case OP_push_i32:
    case OP_push_const: case OP_push_const8:
    case OP_push_true: case OP_push_false:
    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
    case OP_get_loc8: case OP_get_loc:
    case OP_get_loc0_loc1:
    case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
    case OP_get_arg:
    case OP_put_loc0: case OP_put_loc1: case OP_put_loc2: case OP_put_loc3:
    case OP_put_loc8: case OP_put_loc:
    case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
    case OP_set_loc8: case OP_set_loc:
    case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod:
    case OP_lt: case OP_lte: case OP_gt: case OP_gte:
    case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
    case OP_neg: case OP_plus: case OP_not: case OP_lnot:
    case OP_and: case OP_or: case OP_xor:
    case OP_shl: case OP_shr: case OP_sar:
    case OP_inc: case OP_dec: case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
    case OP_is_undefined_or_null:
    case OP_to_propkey: case OP_to_propkey2:
    case OP_goto: case OP_goto8: case OP_goto16:
    case OP_if_false: case OP_if_false8: case OP_if_true: case OP_if_true8:
    case OP_drop: case OP_dup: case OP_nip: case OP_nip1:
    case OP_return: case OP_return_undef:
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

/* The local an instruction writes, or -1. put/set/add/inc/dec and the
   check/uninitialized stores all write; reads and closure-cell writes do
   not. Opcodes the generator does not handle need no entry: they bail the
   whole function, so the written-set is never consulted for them. */
static int whale_writes_local(int op, const uint8_t *pc, int pos)
{
    if (op >= OP_put_loc0 && op <= OP_put_loc3)
        return op - OP_put_loc0;
    if (op >= OP_set_loc0 && op <= OP_set_loc3)
        return op - OP_set_loc0;
    if (op == OP_put_loc8 || op == OP_set_loc8 ||
        op == OP_inc_loc || op == OP_dec_loc || op == OP_add_loc)
        return (int)pc[pos + 1];
    if (op == OP_put_loc || op == OP_set_loc ||
        op == OP_put_loc_check || op == OP_put_loc_check_init ||
        op == OP_set_loc_uninitialized)
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

        if (whale_dbg_env("WHALE_JIT_DUMP"))
            fprintf(stderr, "[feeds] k=%d o=%d dup=%d push=%d\n",
                    k, o, OP_dup, whale_is_push_op(o));
        if (whale_is_call_op(o))
            return 1;
        /* Two opcodes carry the callee or its arguments rather than starting
           something new, and neither is in whale_is_push_op:
             - `dup`: a constructor call is `get_var F; dup; <args>; call_constructor`,
               the bytecode needing F twice - as the function and as new.target;
             - `push_atom_value`: a string argument, exactly the case in
               `throw new Error("negative")`.
           Stopping at either typed the callee as a number, and the whole function
           was then refused. */
        if (o != OP_dup && o != OP_push_atom_value && !whale_is_push_op(o))
            return 0;
        s = whale_op_size_of(o);
        if (s <= 0)
            return 0;
        k += s;
        n++;
    }
    return 0;
}

/* Is this argument used as an *array* - `.length`, indexed in or out?
   `a.length` is adjacent; `a[i]` is get_arg0 a, get_loc0 i, get_array_el, so
   the container sits under the one pushed index; `out[i] = a` is put_array_el
   under two. The walker counts pure pushes and answers only when an array use
   appears with this argument as its container. Being wrong is safe: the
   argument then travels as a plain number and any object use deopts. */
static int whale_feeds_array_use(const uint8_t *pc, int len, int pos, int sz)
{
    int k = pos + sz;
    int n = 0;

    while (k < len && n < 5) {
        int o = (int)pc[k];
        int s;

        if (o == OP_get_length && n == 0)
            return 1;
        if (o == OP_get_array_el && n == 1)
            return 1;
        if (o == OP_put_array_el && n == 2)
            return 1;
        if (o != OP_dup && o != OP_push_atom_value && !whale_is_push_op(o))
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
       is already an in-range int32. IBIT is the same promise in bare-int form
       (a bitwise result without the `(double)` wrapper). */
    return t == WHALE_T_NUM || t == WHALE_T_BOOL || t == WHALE_T_I32 ||
           t == WHALE_T_ICONST || t == WHALE_T_IBIT;
}

/* An int-clean operand: its value is already an int32, so a bitwise op can
   consume it with at most a cast - and when every operand of a bitwise op is
   int-clean the whole op can stay a bare int expression (WHALE_T_IBIT). */
static int whale_type_is_intclean(int t)
{
    return t == WHALE_T_I32 || t == WHALE_T_ICONST || t == WHALE_T_IBIT;
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
/* A conditional jump seen from the instruction before it: opcode, target and
   polarity, or 0 when the next instruction is not one. Lets a comparison fold
   itself into the branch (`cmp; if` -> one `if (!(a<b)) goto`) instead of
   materialising a boolean double first. */
static int whale_next_is_branch(const uint8_t *pc, int len, int pos, int sz,
                                int *tgt_out, int *is_true_out, int *isz_out)
{
    int nop, ntgt, nis_true, nisz;

    if (pos + sz >= len)
        return 0;
    nop = pc[pos + sz];
    if (nop == OP_if_false8 || nop == OP_if_true8) {
        nis_true = (nop == OP_if_true8);
        ntgt = pos + sz + 1 + (int8_t)pc[pos + sz + 1];
        nisz = 2;
    } else if (nop == OP_if_false || nop == OP_if_true) {
        nis_true = (nop == OP_if_true);
        ntgt = pos + sz + 1 + (int32_t)whale_get_u32(pc + pos + sz + 1);
        nisz = 5;
    } else {
        return 0;
    }
    if (ntgt < 0 || ntgt > len)
        return 0;
    *tgt_out = ntgt;
    *is_true_out = nis_true;
    *isz_out = nisz;
    return 1;
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

/* A token range in the folding workspace that produces one value. inv means
   every leaf is loop-invariant (a literal, a parameter, or a constant local):
   the value is the same on every iteration, so a materialisation inside a
   loop body can go before the loop instead. Set only where the source is
   known (literal pushes, get_arg, constant-local reads); binop takes the AND
   of its operands. Everything else defaults to 0, which is always safe. */
typedef struct { int off, len; uint8_t inv; } WhaleVExpr;

/* A local can be deferred like a stack slot (see the deferred-locals note next
   to the helpers). Declared here because whale_load_analyze() below has to know
   whether a local currently holds a pending expression rather than a value. */
typedef struct WhaleLocal {
    WhaleVExpr expr;
    uint8_t    live;    /* 1 = pending expression, 0 = read the variable */
    uint8_t    used;    /* the pending expression has been inlined once */
} WhaleLocal;

static WhaleLocal *whale_lmem;

/* Int-region mirrors, indexed by local: 1 = local d runs on a C `int` mirror
   (I<d>) inside the recognised loop, 0 = plain double path. Valid only when
   whale_int_ok is set (exactly one int-region in the function); zeroed per
   compilation next to the loop scans. Up here (not with the other loop
   state) because whale_load_analyze() below must refuse mirror reads. */
static uint8_t whale_int_mirror[WHALE_JIT_MAX_SLOTS];
static int whale_int_ok;
static int whale_int_head;
static int whale_int_exit;
static int whale_int_g;
static int whale_int_need_guard;
static int whale_int_k_arg;
/* Current loop-buffer depth (the WhaleLoop stack itself stays below with the
   other loop state). Needed here for the same mirror refusal: outside any
   loop the exit edge has boxed the mirror back, so a plain L read is right. */
static int whale_loop_depth;

/* Decide what this load means. Returns 0 if it is not a simple load. */
static int whale_load_analyze(const JSWhaleFuncInfo *info, const uint8_t *pc, int pos, const uint8_t *cur,
                              int var_count, const uint8_t *loc_c, const double *loc_cv,
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
               the switch path goes through local_inline() instead. Exception:
               a constant local reads as its literal either way, so there is
               nothing stale to protect. */
            if (idx >= 0 && idx < var_count && whale_lmem && whale_lmem[idx].live &&
                !(idx >= 0 && idx < var_count && loc_c && loc_c[idx]))
                return 0;
            /* An int-region mirror must not be read as `L<idx>` inside the
               loop - the loop runs on I<idx> (the head test reads the mirror
               while the back edge steps it; mixing the two is an infinite
               loop). Refuse, so the switch path emits the mirror read. A
               constant local is still a literal either way. */
            if (idx >= 0 && idx < WHALE_JIT_MAX_SLOTS && whale_int_ok &&
                whale_int_mirror[idx] && whale_loop_depth > 0 &&
                !(idx >= 0 && idx < var_count && loc_c && loc_c[idx]))
                return 0;
            *type_out = tt;
            /* constant propagation: a local written exactly once with a
               compile-time constant is that constant everywhere afterwards */
            if (idx >= 0 && idx < var_count && loc_c && loc_c[idx]) {
                *kind_out = WLD_REAL;
                *cval_out = loc_cv[idx];
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
/* The body buffer has to be as large as the output it stands in for. It was 4096,
   and the append itself was bounded by WHALE_TOK_MAX (32768), so a body of more
   than 4096 tokens ran off the end of l->tok - which is why this was switched off.
   Same bound as go_tok is the only value that is safe by construction: while a body
   is being built, whale_out_tok points here and every writer still checks against
   WHALE_TOK_MAX. */
#define WHALE_LOOP_BUF WHALE_TOK_MAX

typedef struct WhaleLoop {
    int     tok[WHALE_LOOP_BUF];
    double  num[WHALE_LOOP_BUF];
    int     len;
    int     head_pos;       /* bytecode offset of the loop head */
    int     back_pos;       /* ...of the backward jump that closes the body */
} WhaleLoop;

static WhaleLoop whale_loops[WHALE_LOOP_MAX];
/* Loop-level stability table (definition; a forward declaration also sits
   next to whale_field_stable below - both are tentative, they merge). */
static uint8_t *whale_loop_stable_tab;
/* 1 when the current emit position sits inside a shape-stable loop body
   (notes <114>). The table is filled once per compilation next to the
   shape_stable computation; the pointer below publishes it to emit time
   (NULL outside gen, cleared with the table at out:). */
static int whale_loop_stable_here(void)
{
    if (whale_loop_stable_tab && whale_loop_depth > 0) {
        int hd = whale_loops[whale_loop_depth - 1].head_pos;

        if (hd >= 0)
            return whale_loop_stable_tab[hd] ? 1 : 0;
    }
    return 0;
}
/* (whale_loop_depth lives above, next to whale_lmem: the load analyser
   needs it before this point in the file.) */
/* Per loop head: which locals its body writes. Indexed [head * n + d], built
   once before the emit pass walks the bytecode. A variable read inside the
   loop is loop-invariant when its bit is clear - the back edge re-runs the
   whole body, so "written anywhere in the body" is exactly "not the same on
   the next trip". Locals can only be written through these opcodes (a local
   is a double, there is no aliasing), which makes the scan complete. */
static uint8_t *whale_loop_written;
static int whale_loop_wn;   /* locals per head entry (var_count) */
/* Set when any loop body contains a call: local pinning is then off (a pin
   spanning a call pays spill+reload per call for zero net gain). */
static int whale_loop_has_call;
/* Set when any loop body creates an object (OP_object) or stores into an
   object-holding local (a second object identity landing in the same slot
   would reshape it): VAL-slot field caches are then not stable. Zeroed per
   compilation next to the loop scans. */
static int whale_loop_shape_dirty;
/* Set when a real bytecode call sits inside a loop body. Unlike has_call
   (which also fires for opcodes that merely lower to numeric helpers like
   fmod/ObjShape - harmless to object shapes), only a bytecode call can hand
   an object out to user code that reshapes it. VAL-slot stability consults
   this, not has_call: every object loop lowers field access to helpers, so
   gating on has_call would close stability for exactly the loops it helps. */
static int whale_loop_has_bcall;
/* Reuse-loop head -> _wsc.ru[] index (4 cap). Assigned on first use per
   compilation; a fifth reuse loop keeps per-trip allocation. Zeroed per
   compilation next to the loop scans (a stale head would reuse the wrong
   slot's mark). */
static int whale_reuse_map[4];
static int whale_reuse_n;
static int whale_reuse_ridx(int head)
{
    int i;

    for (i = 0; i < whale_reuse_n && i < 4; i++)
        if (whale_reuse_map[i] == head)
            return i;
    if (whale_reuse_n >= 4)
        return -1;
    whale_reuse_map[whale_reuse_n] = head;
    return whale_reuse_n++;
}
/* Per-local heat inside loop bodies (reads + 2*writes, accumulated over all
   loops): the ELS decision pins the hottest ones. Indexed by local number,
   zeroed before each compilation's scan. */
static int whale_loop_heat[WHALE_JIT_MAX_SLOTS];
/* (whale_int_mirror and friends live above, next to whale_lmem: the load
   analyser below needs them before this point in the file.) */

/* Presence-flag check dedup, one straight-line segment at a time. Bit col of
   the innermost open loop's word means "sp[col] is already proven 1 on this
   path" - by an earlier check or by a store that set it. A later read of the
   same column then skips its `if (!sp[col])` branch. Sound because sp[] only
   ever goes 0->1 (stores set, nothing clears except a fresh OP_object, which
   resets the words outright), and because the word resets at every jump
   target - so a set bit always dominates the read it excuses. In particular a
   check inside one `if` branch never excuses a read after the join. */
static unsigned whale_sp_seen[WHALE_LOOP_MAX];

static int whale_sp_known(int col)
{
    if (col < 0 || col >= 32)
        return 0;
    if (whale_loop_depth <= 0 || whale_loop_depth > WHALE_LOOP_MAX)
        return 0;
    return (whale_sp_seen[whale_loop_depth - 1] & (1u << col)) != 0;
}

static void whale_sp_note(int col)
{
    if (col < 0 || col >= 32)
        return;
    if (whale_loop_depth <= 0 || whale_loop_depth > WHALE_LOOP_MAX)
        return;
    whale_sp_seen[whale_loop_depth - 1] |= 1u << col;
}

/* A new straight-line segment starts: nothing is proven on it yet. */
static void whale_sp_reset(void)
{
    if (whale_loop_depth <= 0 || whale_loop_depth > WHALE_LOOP_MAX)
        return;
    whale_sp_seen[whale_loop_depth - 1] = 0;
}

/* A fresh object clears every presence flag, on every open level: a check
   proven before the creation says nothing about the new object. Creation is
   rare (literals live before loops), so wiping all levels is cheap. */
static void whale_sp_reset_all(void)
{
    int d;

    for (d = 0; d < WHALE_LOOP_MAX; d++)
        whale_sp_seen[d] = 0;
}

/* Is local idx loop-invariant at the current position: never written inside
   the innermost loop being generated? Outside any loop the answer does not
   matter (nothing hoists there), so it reads 0. Only the innermost loop is
   consulted: hoisting moves the value to the directly enclosing output, where
   it is recomputed once per outer trip - correct even if an outer loop body
   writes the local. */
static int whale_local_invariant(int idx)
{
    int head;

    if (idx < 0 || idx >= whale_loop_wn)
        return 0;
    if (whale_loop_depth <= 0 || !whale_loop_written)
        return 0;
    head = whale_loops[whale_loop_depth - 1].head_pos;
    if (head < 0)
        return 0;
    return !whale_loop_written[(size_t)head * whale_loop_wn + idx];
}

/* Function loop count for the pin gate below (fold_materialize has no info).
   Set once per compilation next to the loop scans. */
static int whale_emit_loop_count;
/* Small-trip-count unrolling: per-head trip count (0 = do not unroll), the
   induction local, and the head test's exit. Indexed by bytecode head
   position like loop_back[]; allocated with it, filled once before the
   emit pass, freed with the rest at out:. */
/* Memset-able loop descriptor: `for (g = C0; g <= K / g < K; g++) arr[g] = C`.
   Filled by whale_loop_memset_of (0 = not this shape). The array's TAU8 slot
   is NOT resolved here (pass 1 has no types) - loop_end confirms the body
   really is a TAU8 store before emitting the call. The bound K is either a
   literal (k_is_arg = 0, k = value) or a parameter (k_is_arg = 1, k_arg =
   its index, read as An[] at run time). */
typedef struct {
    int g;        /* induction local */
    double c0;    /* start (a literal) */
    double k;     /* bound literal (when k_is_arg == 0) */
    int k_is_arg; /* bound is parameter k_arg */
    int k_arg;    /* bound parameter index */
    int le;       /* 1 = `<=` (inclusive), 0 = `<` */
    double c;     /* stored value (a 0..255 integer literal) */
    int exitp;    /* head test's exit */
} WhaleMemsetLoop;

/* Count-reduction loop descriptor: `for (g = C0; g <= K; g++) if (arr[g])
   acc++` over a Uint8Array. Filled by whale_loop_count_of (ok = recognised).
   Like the memset descriptor, the array slot is confirmed from the body
   tokens at loop_end, never here. */
typedef struct {
    int ok;       /* recognised (calloc zeroes impersonate local 0) */
    int g;        /* induction local */
    int acc;      /* accumulation local (inc_loc target, != g) */
    double c0;    /* start (a literal) */
    double k;     /* bound literal (when k_is_arg == 0) */
    int k_is_arg; /* bound is parameter k_arg */
    int k_arg;    /* bound parameter index */
    int le;       /* 1 = `<=` (inclusive), 0 = `<` */
    int exitp;    /* head test's exit */
} WhaleCountLoop;

static WhaleMemsetLoop *whale_loop_memset;
static WhaleCountLoop *whale_loop_countrec;

/* In-loop object-reuse descriptor: a loop body that builds exactly one fresh
   object (`OP_object` + numeric define/put_field only) and parks it into a
   write-only array (no element/length reads of that array anywhere, no call,
   no return/global store of either). The object slot is then reused across
   trips: first trip builds (ObjNew + Define = transition + store), steady
   trips only store values (StoreValue, shape unchanged, no transition, no
   prop-array growth). The parked array keeps length semantics (ArrSetVal
   stays); only the per-trip allocation dies. Like memset/count, the array
   slot is confirmed from the body tokens at emit time, never here. */
typedef struct {
    int ok;       /* recognised (calloc zeroes impersonate head 0) */
    int objpos;   /* OP_object position (the reused slot's creation site) */
    int arrpos;   /* put_array_el position (the parking store) */
    int ridx;     /* _wsc.ru[] index (assigned at fill time, per head) */
} WhaleReuseLoop;

static WhaleReuseLoop *whale_loop_reuse;
static int *whale_loop_trip;
static int *whale_loop_trip_g;
static int *whale_loop_trip_exit;

/* Integer-region loop descriptor: `for (g = 0; g < K; g++) <int-pure body>`.
   The induction local g and every `put_loc v` whose value is produced by an
   int bitwise op get a C `int` mirror (I<d>) for the whole loop trip: reads
   and writes inside the loop run on the mirror with zero conversion, and the
   exit edge boxes each mirror back once (`L<d> = (double)I<d>`). The bound K
   is a literal (<= 2^30, so `I<g>++` can never overflow) or a parameter (then
   need_guard = 1 and the entry emits `if (K > 2^30) deopt`). `<=` is not taken
   yet (exit value g = K+1 needs the same boxing, just unwired).

   Only the narrow shape is recognised: `g = 0` right before the head, head
   test `get_loc g; X; lt; if_false exit`, `inc_loc g` at back-2, `goto head`
   at back, no call, no other write to g, no exit except the head test's, and
   every mirror candidate written only by int-producing puts and read only
   next to bitwise consumers (see the triple check below). Anything else
   refuses and the loop is emitted as before. */
typedef struct {
    int ok;       /* recognised (calloc zeroes impersonate local 0) */
    int g;        /* induction local */
    double k;     /* bound literal (when k_is_arg == 0) */
    int k_is_arg; /* bound is parameter k_arg */
    int k_arg;    /* bound parameter index */
    int exitp;    /* head test's exit */
    int need_guard; /* bound is a parameter: entry guard required */
    uint8_t mirror[WHALE_JIT_MAX_SLOTS]; /* 1 = int mirror (g + int-pure puts) */
} WhaleIntLoop;

static WhaleIntLoop *whale_loop_intreg;

/* A small integer literal push at pos: value in *out, length in *ln. Only the
   immediate forms (push_0..7/i8/i16); the constant pool is needless here. */
static int whale_push_lit(const uint8_t *pc, int len, int pos, double *out, int *ln)
{
    int op;

    if (pos < 0 || pos >= len)
        return 0;
    op = pc[pos];
    if (op >= OP_push_0 && op <= OP_push_7) {
        *out = (double)(op - OP_push_0);
        *ln = 1;
        return 1;
    }
    if (op == OP_push_i8 && pos + 1 < len) {
        *out = (double)(int)(int8_t)pc[pos + 1];
        *ln = 2;
        return 1;
    }
    if (op == OP_push_i16 && pos + 2 < len) {
        *out = (double)(int)(int16_t)whale_get_u16(pc + pos + 1);
        *ln = 3;
        return 1;
    }
    if (op == OP_push_minus1) {
        *out = -1;
        *ln = 1;
        return 1;
    }
    return 0;
}

/* A get_loc at pos: local index in *out, length in *ln. */
static int whale_get_loc_at(const uint8_t *pc, int len, int pos, int var_count,
                            int *out, int *ln)
{
    int op, idx = -1, l = 0;

    if (pos < 0 || pos >= len)
        return 0;
    op = pc[pos];
    if (op >= OP_get_loc0 && op <= OP_get_loc3) {
        idx = op - OP_get_loc0;
        l = 1;
    } else if (op == OP_get_loc8 && pos + 1 < len) {
        idx = (int)pc[pos + 1];
        l = 2;
    } else if (op == OP_get_loc && pos + 2 < len) {
        idx = (int)whale_get_u16(pc + pos + 1);
        l = 3;
    } else {
        return 0;
    }
    if (idx < 0 || idx >= var_count)
        return 0;
    *out = idx;
    *ln = l;
    return 1;
}

/* Trip count of the loop [head, back], or 0. Recognises only
   `g = 0; head: ... g < K ...; inc g; goto head` with K <= 8, a single
   incrementing write, no call, and no exit except the head test's. Every
   rejection is conservative: an unrecognised loop is emitted as before. */
static int whale_loop_trip_of(const uint8_t *pc, int len, int head, int back,
                              int var_count, const int *loop_back,
                              const uint8_t *targets,
                              const int *call_pos, int call_n,
                              int *g_out, int *exit_out)
{
    int g = -1, gl = 0, kl = 0, cond, ifop, exitp = -1;
    double kv = 0;
    int q, qsz, writes = 0;

    if (head < 0 || back <= head || back >= len)
        return 0;
    /* back edge is `...; inc_loc g; goto head` with the goto at `back`.
       Target = opcode offset + 1 + displacement (the interpreter's pc points
       at the operand when it adds, same as pass 1's loop_back fill). */
    {
        int nop = pc[back], ntgt = -1;

        if (nop == OP_goto8 && back + 1 < len)
            ntgt = back + 1 + (int8_t)pc[back + 1];
        else if (nop == OP_goto && back + 4 < len)
            ntgt = back + 1 + (int32_t)whale_get_u32(pc + back + 1);
        else
            return 0;
        if (ntgt != head)
            return 0;
    }
    if (back < 2 || pc[back - 2] != OP_inc_loc)
        return 0;
    g = (int)pc[back - 1];
    if (g < 0 || g >= var_count)
        return 0;
    /* head test is `get_loc g; push K; lt; if_false exit` */
    {
        int gg = -1;

        if (!whale_get_loc_at(pc, len, head, var_count, &gg, &gl) || gg != g)
            return 0;
    }
    {
        int kp = head + gl;

        if (!whale_push_lit(pc, len, kp, &kv, &kl))
            return 0;
        if (kv < 1 || kv > 8 || kv != (double)(int)kv)
            return 0;
    }
    {
        int lp = head + gl + kl;

        if (lp >= len || pc[lp] != OP_lt)
            return 0;
        {
            int ip = lp + 1;

            if (ip >= len)
                return 0;
            ifop = pc[ip];
            if (ifop == OP_if_false8 && ip + 1 < len)
                exitp = ip + 1 + (int8_t)pc[ip + 1];
            else if (ifop == OP_if_false && ip + 4 < len)
                exitp = ip + 1 + (int32_t)whale_get_u32(pc + ip + 1);
            else
                return 0;
            if (exitp < 0 || exitp > len)
                return 0;
        }
    }
    /* `g = 0` right before the head */
    {
        int pp = -1, vp = -1, vl = 0;

        if (head < 2)
            return 0;
        if (pc[head - 1] >= OP_put_loc0 && pc[head - 1] <= OP_put_loc3) {
            vp = pc[head - 1] - OP_put_loc0;
            pp = head - 2;
        } else if (head >= 3 && pc[head - 2] == OP_put_loc8) {
            vp = (int)pc[head - 1];
            pp = head - 3;
        } else {
            return 0;
        }
        if (vp != g)
            return 0;
        if (!whale_push_lit(pc, len, pp, &kv, &vl) || kv != 0)
            return 0;
        (void)cond;
    }
    /* single incrementing write, no call, no foreign exit inside */
    for (q = head; q <= back; ) {
        int qo = (int)pc[q];
        int w;

        qsz = whale_op_size_of(qo);
        if (qsz <= 0 || q + qsz > len)
            return 0;
        if (qo == OP_goto8 || qo == OP_goto || qo == OP_if_false8 ||
            qo == OP_if_false || qo == OP_if_true8 || qo == OP_if_true) {
            int t2 = -1;

            if ((qo == OP_goto8 || qo == OP_if_false8 || qo == OP_if_true8) &&
                q + 1 < len)
                t2 = q + 1 + (int8_t)pc[q + 1];
            else if (q + 4 < len)
                t2 = q + 1 + (int32_t)whale_get_u32(pc + q + 1);
            if (t2 < head || t2 > back) {
                /* leaving the body: only the head test's exit and the back
                   edge's return to head are allowed */
                int is_head_test = (q + qsz == head + gl + kl + 1 + 2) ||
                                   (q + qsz == head + gl + kl + 1 + 5);
                int is_back_edge = (q == back);

                if (!((t2 == exitp && is_head_test) || (t2 == head && is_back_edge)))
                    return 0;
            }
        }
        w = whale_writes_local(qo, pc, q);
        if (w == g) {
            if (q == back - 2)
                writes++;
            else
                return 0;
        }
        q += qsz;
    }
    if (writes != 1)
        return 0;
    {
        int k;

        for (k = 0; k < call_n; k++)
            if (call_pos[k] >= head && call_pos[k] <= back)
                return 0;
    }
    /* nested loops inside are fine to leave looping (only the innermost with
       a proven trip is expanded); but an inner loop head inside would need
       its own buffer handling - the expander only takes a body with no
       inner loop head of its own. */
    {
        int qq;

        for (qq = head + 1; qq <= back; qq++)
            if (loop_back[qq] >= 0)
                return 0;
    }
    *g_out = g;
    *exit_out = exitp;
    {
        double k2 = 0;
        int kl2 = 0;

        if (!whale_push_lit(pc, len, head + gl, &k2, &kl2))
            return 0;
        return (int)k2;
    }
}

/* A `get_loc v` at q whose value feeds a bitwise consumer directly: either a
   single-operand int op right after (`get_loc v; not`), or a binary int op
   with the other operand loaded in between (`get_loc v; push K; and`). Only
   the int-bitwise family plus mod qualifies - `+ - * /` and comparisons take
   doubles, and `>>>` (shr) can exceed int32. Returns 1 with the consumer
   opcode in *cop when the triple matches. */
static int whale_int_consumer_at(const uint8_t *pc, int len, int q, int *cop)
{
    int n1 = (q + 1 < len) ? pc[q + 1] : -1;
    int n2 = (q + 2 < len) ? pc[q + 2] : -1;
    int n3 = (q + 3 < len) ? pc[q + 3] : -1;

    /* single-operand: `get_loc v; not` (lnot is `!`, excluded: boolean) */
    if (n1 == OP_not) {
        *cop = n1;
        return 1;
    }
    /* binary: `get_loc v; <right-operand load>; <binop>`. The right operand
       is any load (push/get/const); the consumer must be int-bitwise or mod.
       `get_loc v; get_loc w; not` (sub's left read followed by another load)
       fails here because not is single-operand, not a binary consumer. */
    if (n1 == OP_push_minus1 ||
        (n1 >= OP_push_0 && n1 <= OP_push_7) || n1 == OP_push_i8 ||
        n1 == OP_push_i16 || n1 == OP_push_i32 || n1 == OP_push_const ||
        n1 == OP_push_const8 || n1 == OP_push_true || n1 == OP_push_false ||
        (n1 >= OP_get_loc0 && n1 <= OP_get_loc3) || n1 == OP_get_loc8 ||
        n1 == OP_get_loc || n1 == OP_get_arg0 || n1 == OP_get_arg1 ||
        n1 == OP_get_arg2 || n1 == OP_get_arg3 || n1 == OP_get_arg) {
        int nl = whale_op_size_of(n1);

        if (nl > 0) {
            int bop = (q + 1 + nl < len) ? pc[q + 1 + nl] : -1;

            if (bop == OP_and || bop == OP_or || bop == OP_xor ||
                bop == OP_shl || bop == OP_sar || bop == OP_mod) {
                *cop = bop;
                return 1;
            }
        }
    }
    (void)n2;
    (void)n3;
    return 0;
}

/* Recognise `for (g = 0; g < K; g++) <int-pure body>` at [head, back] and
   fill the int-region descriptor: the induction local plus every `put_loc v`
   produced by an int bitwise op and consumed only by bitwise triples (see
   whale_int_consumer_at). `set_loc`/`add_loc` writes to a candidate refuse
   (set leaves a double-meaning value on the stack); `+ - * /`, comparisons
   consuming a candidate refuse (they need doubles); `>>>` refuses (exceeds
   int32). The bound is a small literal (<= 2^30, no overflow possible) or a
   parameter (need_guard = 1). Every rejection is conservative. */
static int whale_loop_int_of(const uint8_t *pc, int len, int head, int back,
                             int var_count, const int *loop_back,
                             const int *call_pos, int call_n,
                             WhaleIntLoop *out)
{
    int g = -1, gl = 0, kl = 0, ifop, exitp = -1;
    double kv = 0;
    int q, qsz;
    int put_prod[WHALE_JIT_MAX_SLOTS];
    int put_cnt[WHALE_JIT_MAX_SLOTS];
    int no_mirror[WHALE_JIT_MAX_SLOTS];
    int i;

    if (head < 0 || back <= head || back >= len)
        return 0;
    /* back edge is `...; inc_loc g; goto head` */
    {
        int nop = pc[back], ntgt = -1;

        if (nop == OP_goto8 && back + 1 < len)
            ntgt = back + 1 + (int8_t)pc[back + 1];
        else if (nop == OP_goto && back + 4 < len)
            ntgt = back + 1 + (int32_t)whale_get_u32(pc + back + 1);
        else
            return 0;
        if (ntgt != head)
            return 0;
    }
    if (back < 2 || pc[back - 2] != OP_inc_loc)
        return 0;
    g = (int)pc[back - 1];
    if (g < 0 || g >= var_count || g >= WHALE_JIT_MAX_SLOTS)
        return 0;
    /* head test is `get_loc g; X; lt; if_false exit` (`<` only) */
    {
        int gg = -1;

        if (!whale_get_loc_at(pc, len, head, var_count, &gg, &gl) || gg != g)
            return 0;
    }
    {
        int kp = head + gl;
        int op;

        if (whale_push_lit(pc, len, kp, &kv, &kl)) {
            if (kv < 1 || kv > 1073741824.0 || kv != (double)(int)kv)
                return 0;
            out->k_is_arg = 0;
            out->k = kv;
            out->need_guard = 0;
        } else {
            op = (kp < len) ? pc[kp] : -1;
            if (op >= OP_get_arg0 && op < OP_get_arg0 + 4) {
                out->k_is_arg = 1;
                out->k_arg = op - OP_get_arg0;
                kl = 1;
            } else if (op == OP_get_arg && kp + 2 < len) {
                out->k_is_arg = 1;
                out->k_arg = (int)whale_get_u16(pc + kp + 1);
                kl = 3;
            } else {
                return 0;
            }
            out->k = 0;
            out->need_guard = 1;
        }
    }
    {
        int lp = head + gl + kl;

        if (lp >= len || pc[lp] != OP_lt)
            return 0;
        {
            int ip = lp + 1;

            if (ip >= len)
                return 0;
            ifop = pc[ip];
            if (ifop == OP_if_false8 && ip + 1 < len)
                exitp = ip + 1 + (int8_t)pc[ip + 1];
            else if (ifop == OP_if_false && ip + 4 < len)
                exitp = ip + 1 + (int32_t)whale_get_u32(pc + ip + 1);
            else
                return 0;
            if (exitp < 0 || exitp > len)
                return 0;
            /* the exit leaves the loop body */
            if (exitp >= head && exitp <= back)
                return 0;
        }
    }
    /* `g = 0` right before the head */
    {
        int pp = -1, vp = -1, vl = 0;
        double zv = 0;

        if (head < 2)
            return 0;
        if (pc[head - 1] >= OP_put_loc0 && pc[head - 1] <= OP_put_loc3) {
            vp = pc[head - 1] - OP_put_loc0;
            pp = head - 2;
        } else if (head >= 3 && pc[head - 2] == OP_put_loc8) {
            vp = (int)pc[head - 1];
            pp = head - 3;
        } else {
            return 0;
        }
        if (vp != g)
            return 0;
        if (!whale_push_lit(pc, len, pp, &zv, &vl) || zv != 0)
            return 0;
    }
    for (i = 0; i < WHALE_JIT_MAX_SLOTS; i++) {
        put_prod[i] = -1;
        put_cnt[i] = 0;
        no_mirror[i] = 0;
    }
    /* body scan: no call, no foreign exit, no inner head; g written only at
       back-2; record each put_loc's producer (the opcode right before it). */
    for (q = head; q <= back; ) {
        int qo = (int)pc[q];
        int w;

        qsz = whale_op_size_of(qo);
        if (qsz <= 0 || q + qsz > len)
            return 0;
        if (qo == OP_goto8 || qo == OP_goto || qo == OP_if_false8 ||
            qo == OP_if_false || qo == OP_if_true8 || qo == OP_if_true) {
            int t2 = -1;

            if ((qo == OP_goto8 || qo == OP_if_false8 || qo == OP_if_true8) &&
                q + 1 < len)
                t2 = q + 1 + (int8_t)pc[q + 1];
            else if (q + 4 < len)
                t2 = q + 1 + (int32_t)whale_get_u32(pc + q + 1);
            if (t2 < head || t2 > back) {
                int is_head_test = (q == head + gl + kl + 1);
                int is_back_edge = (q == back);

                if (!((t2 == exitp && is_head_test) || (t2 == head && is_back_edge)))
                    return 0;
            }
        }
        w = whale_writes_local(qo, pc, q);
        if (w == g) {
            if (q == back - 2)
                ; /* the induction increment */
            else
                return 0;
        } else if (w >= 0) {
            /* set_loc/add_loc leave a double-meaning value on the stack (set)
               or read-modify-write a double (add): that local can never run
               on an int mirror - blacklist it, but keep the loop. Only g
               must have the single induction write. bitops' acc (set_loc0
               after xor/or, then `+`-polluted) is exactly this case. */
            if (qo == OP_put_loc || qo == OP_put_loc8 ||
                (qo >= OP_put_loc0 && qo <= OP_put_loc3)) {
                if (w >= WHALE_JIT_MAX_SLOTS)
                    return 0;
                put_cnt[w]++;
                /* producer = opcode ending right at q (the value on top).
                   Only single-byte puts can read it off at q-1: a put_loc8's
                   q-1 is its own slot operand, not the producer. Multi-byte
                   puts keep -2 (double path, always safe). */
                if (qo >= OP_put_loc0 && qo <= OP_put_loc3 && q > head) {
                    int po = (int)pc[q - 1];

                    /* single-byte producers (all int bitwise ops are) */
                    if (po == OP_and || po == OP_or || po == OP_xor ||
                        po == OP_shl || po == OP_sar || po == OP_not)
                        put_prod[w] = po;
                    else
                        put_prod[w] = -2; /* non-int producer */
                } else {
                    put_prod[w] = -2;
                }
            } else {
                /* set/add/inc/dec on a non-g local: blacklist the mirror but
                   keep the loop (see above). */
                if (w >= WHALE_JIT_MAX_SLOTS)
                    return 0;
                no_mirror[w] = 1;
            }
        }
        q += qsz;
    }
    {
        int k;

        for (k = 0; k < call_n; k++)
            if (call_pos[k] >= head && call_pos[k] <= back)
                return 0;
    }
    {
        int qq;

        for (qq = head + 1; qq <= back; qq++)
            if (loop_back[qq] >= 0)
                return 0;
    }
    /* mirror set: g plus puts produced by int bitwise ops. `mod` is not a
       producer (its emitter may keep fmod on the cold branch - a double),
       but it is a consumer (its fast path takes ints). */
    out->mirror[g] = 1;
    /* g's own reads must also be int-clean: the head test's `get_loc g` is
       the bound compare (exempt - `I<g> < X` promotes exactly like JS), but
       any other read next to a `+` or comparison refuses the whole loop. */
    {
        int qq2, ok = 1;

        for (qq2 = head; qq2 <= back && ok; ) {
            int qo2 = (int)pc[qq2];
            int qsz2 = whale_op_size_of(qo2);
            int r = -1, cop = -1;

            if (qsz2 <= 0 || qq2 + qsz2 > len) {
                ok = 0;
                break;
            }
            if (qo2 >= OP_get_loc0 && qo2 <= OP_get_loc3)
                r = qo2 - OP_get_loc0;
            else if (qo2 == OP_get_loc8 && qq2 + 1 < len)
                r = (int)pc[qq2 + 1];
            else if (qo2 == OP_get_loc && qq2 + 2 < len)
                r = (int)whale_get_u16(pc + qq2 + 1);
            if (r == g && qq2 != head) {
                if (!whale_int_consumer_at(pc, len, qq2, &cop))
                    return 0;
            }
            qq2 += qsz2;
        }
    }
    for (i = 0; i < var_count && i < WHALE_JIT_MAX_SLOTS; i++) {
        int po;

        if (i == g || put_cnt[i] == 0 || no_mirror[i])
            continue;
        po = put_prod[i];
        if (po != OP_and && po != OP_or && po != OP_xor && po != OP_shl &&
            po != OP_sar && po != OP_not)
            continue;
        /* every read of i inside the body must sit next to a bitwise
           consumer (triple check); a `+` or comparison load refuses. */
        {
            int qq2, ok = 1;

            for (qq2 = head; qq2 <= back && ok; ) {
                int qo2 = (int)pc[qq2];
                int qsz2 = whale_op_size_of(qo2);
                int r = -1, cop = -1;

                if (qsz2 <= 0 || qq2 + qsz2 > len) {
                    ok = 0;
                    break;
                }
                if (qo2 >= OP_get_loc0 && qo2 <= OP_get_loc3)
                    r = qo2 - OP_get_loc0;
                else if (qo2 == OP_get_loc8 && qq2 + 1 < len)
                    r = (int)pc[qq2 + 1];
                else if (qo2 == OP_get_loc && qq2 + 2 < len)
                    r = (int)whale_get_u16(pc + qq2 + 1);
                if (r == i) {
                    /* the head test's own `get_loc g` is the bound compare,
                       not a bitwise consumer - g is exempt, candidates are
                       never read there (their store comes later anyway). */
                    if (!whale_int_consumer_at(pc, len, qq2, &cop))
                        ok = 0;
                }
                qq2 += qsz2;
            }
            if (!ok)
                continue;
        }
        out->mirror[i] = 1;
    }
    out->ok = 1;
    out->g = g;
    out->exitp = exitp;
    return 1;
}

/* Recognise `for (g = C0; g <= K; g++) if (arr[g]) acc++` at [head, back].
   The body must be exactly: the bound test, one `arr[g]` read
   (`get_loc arr; get_loc g; get_array_el` adjacent - reads have no
   stack-shuffling prologue, unlike stores), one `if_false` skipping the
   increment, one `inc_loc acc`, the induction increment, and the back edge.
   acc must differ from g. Everything else refuses. */
static int whale_loop_count_of(const uint8_t *pc, int len, int head, int back,
                               int var_count, const int *loop_back,
                               const int *call_pos, int call_n,
                               WhaleCountLoop *out)
{
    int g = -1, gl = 0, kl = 0, ip = -1, ifop = -1, exitp = -1, le = -1;
    double kv = 0, c0v = 0;
    int q, qsz, writes = 0, gets = 0, ifs = 0, incs = 0, acc = -1;
    int body;

    if (head < 0 || back <= head || back >= len)
        return 0;
    {
        int nop = pc[back], ntgt = -1;

        if (nop == OP_goto8 && back + 1 < len)
            ntgt = back + 1 + (int8_t)pc[back + 1];
        else if (nop == OP_goto && back + 4 < len)
            ntgt = back + 1 + (int32_t)whale_get_u32(pc + back + 1);
        else
            return 0;
        if (ntgt != head)
            return 0;
    }
    if (back < 2 || pc[back - 2] != OP_inc_loc)
        return 0;
    g = (int)pc[back - 1];
    if (g < 0 || g >= var_count)
        return 0;
    {
        int gg = -1;

        if (!whale_get_loc_at(pc, len, head, var_count, &gg, &gl) || gg != g)
            return 0;
    }
    {
        int kp = head + gl;

        if (whale_push_lit(pc, len, kp, &kv, &kl)) {
            if (kv < 1 || kv > 2147483647.0 || kv != (double)(int)kv)
                return 0;
            out->k_is_arg = 0;
            out->k = kv;
        } else {
            int op = (kp < len) ? pc[kp] : -1;

            if (op >= OP_get_arg0 && op < OP_get_arg0 + 4) {
                out->k_is_arg = 1;
                out->k_arg = op - OP_get_arg0;
                kl = 1;
            } else if (op == OP_get_arg && kp + 2 < len) {
                out->k_is_arg = 1;
                out->k_arg = (int)whale_get_u16(pc + kp + 1);
                kl = 3;
            } else {
                return 0;
            }
            out->k = 0;
        }
    }
    {
        int lp = head + gl + kl;

        if (lp >= len)
            return 0;
        if (pc[lp] == OP_lt)
            le = 0;
        else if (pc[lp] == OP_lte)
            le = 1;
        else
            return 0;
        ip = lp + 1;
        if (ip >= len)
            return 0;
        ifop = pc[ip];
        if (ifop == OP_if_false8 && ip + 1 < len)
            exitp = ip + 1 + (int8_t)pc[ip + 1];
        else if (ifop == OP_if_false && ip + 4 < len)
            exitp = ip + 1 + (int32_t)whale_get_u32(pc + ip + 1);
        else
            return 0;
        if (exitp < 0 || exitp > len)
            return 0;
    }
    {
        int vp = -1, vl = 0, pp = -1;

        if (head < 2)
            return 0;
        if (pc[head - 1] >= OP_put_loc0 && pc[head - 1] <= OP_put_loc3) {
            vp = pc[head - 1] - OP_put_loc0;
            pp = head - 2;
        } else if (head >= 3 && pc[head - 2] == OP_put_loc8) {
            vp = (int)pc[head - 1];
            pp = head - 3;
        } else {
            return 0;
        }
        if (vp != g)
            return 0;
        if (!whale_push_lit(pc, len, pp, &c0v, &vl) || c0v < 0 ||
            c0v > 2147483647.0 || c0v != (double)(int)c0v)
            return 0;
        if (!out->k_is_arg && (le ? c0v > kv : c0v >= kv))
            return 0;
    }
    body = ip + (ifop == OP_if_false8 ? 2 : 5);
    for (q = body; q <= back; ) {
        int qo = (int)pc[q];

        qsz = whale_op_size_of(qo);
        if (qsz <= 0 || q + qsz > len)
            return 0;
        if (qo == OP_goto8 || qo == OP_goto || qo == OP_if_false8 ||
            qo == OP_if_false || qo == OP_if_true8 || qo == OP_if_true) {
            int t2 = -1;

            if ((qo == OP_goto8 || qo == OP_if_false8 || qo == OP_if_true8) &&
                q + 1 < len)
                t2 = q + 1 + (int8_t)pc[q + 1];
            else if (q + 4 < len)
                t2 = q + 1 + (int32_t)whale_get_u32(pc + q + 1);
            if (t2 < head || t2 > back) {
                int is_back_edge = (q == back);

                if (!(t2 == head && is_back_edge))
                    return 0;
            } else {
                /* inner join (the increment skip): must stay inside */
                ifs++;
            }
            q += qsz;
            continue;
        }
        if (qo == OP_get_array_el) {
            /* `arr[g]` read: `get_loc arr; get_loc g` immediately before.
               Reads have no shuffling prologue, so adjacency holds: try the
               three possible load lengths backwards for both. */
            int g2 = -1, gl2 = 0, found = 0, off;

            if (gets != 0)
                return 0;
            for (off = 1; off <= 3 && !found; off++) {
                int s = q - off;

                if (s >= body &&
                    whale_get_loc_at(pc, len, s, var_count, &g2, &gl2) &&
                    s + gl2 == q && g2 == g) {
                    int a2 = -1, al2 = 0, foff, ok = 0;

                    for (foff = 1; foff <= 3; foff++) {
                        int cs = s - foff;

                        if (cs >= body &&
                            whale_get_loc_at(pc, len, cs, var_count, &a2,
                                             &al2) &&
                            cs + al2 == s) {
                            ok = 1;
                            break;
                        }
                        a2 = -1;
                    }
                    if (ok)
                        found = 1;
                    else
                        g2 = -1;
                } else {
                    g2 = -1;
                }
            }
            if (!found)
                return 0;
            gets++;
            q += qsz;
            continue;
        }
        if (qo == OP_inc_loc) {
            int w = (int)pc[q + 1];

            if (q == back - 2) {
                if (w != g)
                    return 0;
                writes++;
            } else {
                if (w == g)
                    return 0;
                if (incs != 0)
                    return 0;
                acc = w;
                incs++;
            }
            q += qsz;
            continue;
        }
        {
            int w = whale_writes_local(qo, pc, q);

            if (w == g)
                return 0;
            if (w >= 0)
                return 0;
        }
        /* the single element read is allowed through (its TAU8 identity is
           confirmed from the body tokens at loop_end, like the memset
           store); everything else must be pure */
        if (qo != OP_get_array_el && !whale_is_pure_num_op(qo))
            return 0;
        q += qsz;
    }
    if (writes != 1 || gets != 1 || incs != 1 || ifs < 1)
        return 0;
    if (acc < 0 || acc >= var_count || acc == g)
        return 0;
    {
        int k;

        for (k = 0; k < call_n; k++)
            if (call_pos[k] >= head && call_pos[k] <= back)
                return 0;
    }
    {
        int qq;

        for (qq = head + 1; qq <= back; qq++)
            if (loop_back[qq] >= 0)
                return 0;
    }
    out->g = g;
    out->acc = acc;
    out->c0 = c0v;
    out->le = le;
    out->exitp = exitp;
    out->ok = 1;
    return 1;
}

/* Recognise a reusable in-loop build at [head, back]: exactly one OP_object,
   then only numeric define_field/put_field/get_field (values, never new
   identities), exactly one put_array_el parking the object, the induction
   increment, and the back edge. No call in the body, no inner head. The
   parked array's local is recorded for the caller-side write-only proof
   (no get_array_el/get_length on it anywhere in the function, no return or
   global/closure store of it or the object - checked by the caller, which
   sees the whole body; here only the local number is reported).
   Every rejection is conservative. */
static int whale_loop_reuse_of(const uint8_t *pc, int len, int head, int back,
                               int var_count, const int *loop_back,
                               const int *call_pos, int call_n,
                               int *objpos_out, int *arrpos_out)
{
    int q, qsz;
    int objpos = -1, arrpos = -1;
    int exitp = -1;
    int nop = -1, ntgt = -1;

    if (head < 0 || back <= head || back >= len)
        return 0;
    nop = pc[back];
    if (nop == OP_goto8 && back + 1 < len)
        ntgt = back + 1 + (int8_t)pc[back + 1];
    else if (nop == OP_goto && back + 4 < len)
        ntgt = back + 1 + (int32_t)whale_get_u32(pc + back + 1);
    else
        return 0;
    if (ntgt != head)
        return 0;
    for (q = head; q <= back; ) {
        int qo = (int)pc[q];

        qsz = whale_op_size_of(qo);
        if (qsz <= 0 || q + qsz > len)
            return 0;
        if (qo == OP_goto8 || qo == OP_goto || qo == OP_if_false8 ||
            qo == OP_if_false || qo == OP_if_true8 || qo == OP_if_true) {
            int t2 = -1;

            if ((qo == OP_goto8 || qo == OP_if_false8 || qo == OP_if_true8) &&
                q + 1 < len)
                t2 = q + 1 + (int8_t)pc[q + 1];
            else if (q + 4 < len)
                t2 = q + 1 + (int32_t)whale_get_u32(pc + q + 1);
            /* leaving the body: the back edge home, plus one exit (the head
               test's) - anything else refuses. */
            if (t2 < head || t2 > back) {
                if (t2 == head && q == back) {
                    q += qsz;
                    continue;
                }
                if (exitp < 0) {
                    exitp = t2;
                    q += qsz;
                    continue;
                }
                return 0;
            }
            q += qsz;
            continue;
        }
        if (qo == OP_object) {
            if (objpos >= 0)
                return 0;
            objpos = q;
            q += qsz;
            continue;
        }
        if (qo == OP_put_array_el) {
            if (arrpos >= 0)
                return 0;
            arrpos = q;
            q += qsz;
            continue;
        }
        /* numeric field init/update/read: values only, no new identity. */
        if (qo == OP_define_field || qo == OP_put_field ||
            qo == OP_get_field || qo == OP_get_field2) {
            q += qsz;
            continue;
        }
        /* the parked array's local read (its reference for the store) and
           any other local traffic: induction, accumulators and pure numerics
           all pass the generic gate below (they are pure-num ops), so no
           special record is needed here. */
        switch (qo) {
        case OP_swap: case OP_dup: case OP_nip: case OP_nip1:
        case OP_drop: case OP_to_propkey: case OP_to_propkey2:
        case OP_is_undefined_or_null:
            break;
        default:
            if (!whale_is_pure_num_op(qo))
                return 0;
            break;
        }
        q += qsz;
    }
    if (objpos < 0 || arrpos < 0)
        return 0;
    {
        int k;

        for (k = 0; k < call_n; k++)
            if (call_pos[k] >= head && call_pos[k] <= back)
                return 0;
    }
    {
        int qq;

        for (qq = head + 1; qq <= back; qq++)
            if (loop_back[qq] >= 0)
                return 0;
    }
    *objpos_out = objpos;
    *arrpos_out = arrpos;
    return 1;
}

/* Write-only/escape proof for a reuse candidate: the parked array (found via
   the put_array_el at arrpos: `get_loc arr` just before its index sequence)
   must never be read in this function (no get_array_el/get_length on it),
   never returned, never stored global/closure, and the function must contain
   no call at all (a call could stash either object anywhere) and no return
   of a non-trivial value (only a constant/local-number return proves the
   objects stay local). Conservative on every doubt: an unprovable function
   simply keeps per-trip allocation. */
static int whale_loop_reuse_writeonly(const uint8_t *pc, int len, int head,
                                      int back, int var_count,
                                      int objpos, int arrpos,
                                      int arg_count,
                                      const uint8_t *args_obj,
                                      const uint8_t *args_arr,
                                      const uint8_t *loc_obj)
{
    int q, qsz;
    int arrloc = -1;

    (void)head;
    (void)back;
    /* the parked array's local: the `array_from` + `put_loc arr` pair that
       built it (function-wide; the put closest after the array_from). The
       put_array_el back-scan cannot tell the array operand from the index
       and value operands (all three are get_loc-shaped), so the creation
       pair - not proximity - names the array. */
    {
        int s, found = 0;

        for (s = 0; s < len; s++) {
            int op = (int)pc[s];

            if (op == OP_array_from) {
                int t = s + (int)whale_op_size_of(op);

                if (t < len) {
                    int nop = (int)pc[t];

                    if (nop >= OP_put_loc0 && nop <= OP_put_loc3) {
                        arrloc = nop - OP_put_loc0;
                        found = 1;
                        break;
                    } else if (nop == OP_put_loc8 && t + 1 < len) {
                        arrloc = (int)pc[t + 1];
                        found = 1;
                        break;
                    } else if (nop == OP_put_loc && t + 2 < len) {
                        arrloc = (int)whale_get_u16(pc + t + 1);
                        found = 1;
                        break;
                    }
                }
            }
        }
        if (!found)
            return 0;
        if (arrloc < 0 || arrloc >= var_count)
            return 0;
    }
    for (q = 0; q < len; ) {
        int qo = (int)pc[q];

        qsz = whale_op_size_of(qo);
        if (qsz <= 0 || q + qsz > len)
            return 0;
        /* any call anywhere refuses (could stash either object). */
        if (qo == OP_call || (qo >= OP_call0 && qo <= OP_call3))
            return 0;
        /* array reads of our array refuse (write-only proof). A get_array_el
           or get_length whose receiver is `get_loc arrloc` refuses; other
           receivers are other arrays and fine. */
        if (qo == OP_get_array_el || qo == OP_get_length) {
            int s, hit = 0;

            for (s = q - 1; s >= 0 && s >= q - 6; s--) {
                int op = (int)pc[s];
                int li = -1;

                if (op >= OP_get_loc0 && op <= OP_get_loc3)
                    li = op - OP_get_loc0;
                else if (op == OP_get_loc8 && s + 1 < len)
                    li = (int)pc[s + 1];
                else if (op == OP_get_loc && s + 2 < len)
                    li = (int)whale_get_u16(pc + s + 1);
                else
                    continue;
                if (li == arrloc) {
                    hit = 1;
                    break;
                }
                break;
            }
            if (hit)
                return 0;
        }
        /* global/closure stores refuse (escape). NB: OP_scope_put_var_init
           shares its number (194) with OP_push_i8 by the short/temp overlap
           design, so it is NOT listed (it would veto every small-int push).
           Scoped stores are still covered: any with-block opcode in the
           function refuses below (with can observe any scope). */
        if (qo == OP_put_var || qo == OP_put_var_init ||
            qo == OP_put_var_ref ||
            (qo >= OP_put_var_ref0 && qo <= OP_put_var_ref3) ||
            qo == OP_put_var_ref_check || qo == OP_put_var_ref_check_init ||
            qo == OP_scope_put_var ||
            qo == OP_with_put_var)
            return 0;
        if (qo == OP_with_get_var || qo == OP_with_delete_var ||
            qo == OP_with_make_ref || qo == OP_with_get_ref ||
            qo == OP_with_get_ref_undef)
            return 0;
        /* return of anything but a constant, a plain numeric local or an
           argument never used as an object/array refuses (the parked objects
           must not leave). A bare `return;` (return_undef) is empty. */
        if (qo == OP_return_undef) {
            q += qsz;
            continue;
        }
        if (qo == OP_return) {
            int pp = q - 1;

            if (pp < 0)
                return 0;
            {
                int pop = (int)pc[pp];
                int ai = -1;

                if ((pop >= OP_get_arg0 && pop < OP_get_arg0 + 4))
                    ai = pop - OP_get_arg0;
                else if (pop == OP_get_arg && pp + 2 < len)
                    ai = (int)whale_get_u16(pc + pp + 1);
                if (ai >= 0) {
                    /* an argument: safe unless it ever served as an object
                       or array (then it could smuggle either out). */
                    if (ai >= arg_count || !args_obj || !args_arr)
                        return 0;
                    if (args_obj[ai] || args_arr[ai])
                        return 0;
                } else if ((pop >= OP_get_loc0 && pop <= OP_get_loc3) ||
                           pop == OP_get_loc8 || pop == OP_get_loc) {
                    /* a local: safe unless it holds an object (locals unify
                       objects to slots; a plain numeric local cannot smuggle
                       the parked identities out). */
                    int li = (pop >= OP_get_loc0 && pop <= OP_get_loc3)
                        ? pop - OP_get_loc0
                        : (pop == OP_get_loc8 && pp + 1 < len)
                        ? (int)pc[pp + 1]
                        : (pop == OP_get_loc && pp + 2 < len)
                        ? (int)whale_get_u16(pc + pp + 1) : -1;

                    if (li < 0 || li >= WHALE_JIT_MAX_SLOTS)
                        return 0;
                    if (loc_obj && loc_obj[li])
                        return 0;
                } else if (!((pop >= OP_push_0 && pop <= OP_push_7) ||
                      pop == OP_push_i8 || pop == OP_push_i16 ||
                      pop == OP_push_i32 || pop == OP_push_const ||
                      pop == OP_push_const8 ||
                      pop == OP_add || pop == OP_sub || pop == OP_mul ||
                      pop == OP_div || pop == OP_mod ||
                      pop == OP_and || pop == OP_or || pop == OP_xor ||
                      pop == OP_shl || pop == OP_shr || pop == OP_sar ||
                      pop == OP_neg || pop == OP_plus ||
                      pop == OP_inc || pop == OP_dec ||
                      pop == OP_not || pop == OP_lnot ||
                      pop == OP_lt || pop == OP_lte || pop == OP_gt ||
                      pop == OP_gte || pop == OP_eq || pop == OP_neq ||
                      pop == OP_strict_eq || pop == OP_strict_neq ||
                      pop == OP_is_undefined_or_null))
                    return 0;
            }
        }
        q += qsz;
    }
    /* the built object itself never leaves through a local: it has none (it
       stays on the stack from OP_object to put_array_el - any put_loc of an
       object in between would be a second identity, and the recogniser only
       allows the shape above where the stack object flows straight through).
       Silence unused warnings for the anchor positions. */
    (void)objpos;
    (void)arrpos;
    return 1;
}

/* Recognise `for (g = C0; g <= K / g < K; g++) arr[g] = C` at [head, back].
   The body must contain nothing but the induction increment, the bound test,
   and one `arr[g] = C` store (loads/pushes feeding exactly that, plus the
   back-edge goto). C must be a 0..255 integer (a byte store); the bound must
   keep the trip under 2^31 (memset takes a size_t, but a huge trip is a
   deopt-guard away from being wrong - refuse it here instead). Like the trip
   recogniser, every rejection is conservative. */
static int whale_loop_memset_of(const uint8_t *pc, int len, int head, int back,
                                int var_count, const int *loop_back,
                                const int *call_pos, int call_n,
                                WhaleMemsetLoop *out)
{
    int g = -1, gl = 0, kl = 0, ip = -1, ifop = -1, exitp = -1, le = -1;
    double kv = 0, c0v = 0, cv = 0;
    int q, qsz, writes = 0, puts = 0;

    if (head < 0 || back <= head || back >= len)
        return 0;
    /* back edge is `...; inc_loc g; goto head` with the goto at `back` */
    {
        int nop = pc[back], ntgt = -1;

        if (nop == OP_goto8 && back + 1 < len)
            ntgt = back + 1 + (int8_t)pc[back + 1];
        else if (nop == OP_goto && back + 4 < len)
            ntgt = back + 1 + (int32_t)whale_get_u32(pc + back + 1);
        else
            return 0;
        if (ntgt != head)
            return 0;
    }
    if (back < 2 || pc[back - 2] != OP_inc_loc)
        return 0;
    g = (int)pc[back - 1];
    if (g < 0 || g >= var_count)
        return 0;
    /* head test is `get_loc g; (push K | get_arg K); (lt|lte); if_false exit`.
       The bound may be a literal or a parameter (sieve bounds by `n`). */
    {
        int gg = -1;

        if (!whale_get_loc_at(pc, len, head, var_count, &gg, &gl) || gg != g)
            return 0;
    }
    {
        int kp = head + gl;
        int ka = -1, kal = 0;

        if (whale_push_lit(pc, len, kp, &kv, &kl)) {
            if (kv < 1 || kv > 2147483647.0 || kv != (double)(int)kv)
                return 0;
            out->k_is_arg = 0;
            out->k = kv;
        } else {
            int op = (kp < len) ? pc[kp] : -1;

            if (op >= OP_get_arg0 && op < OP_get_arg0 + 4) {
                ka = op - OP_get_arg0;
                kal = 1;
            } else if (op == OP_get_arg && kp + 2 < len) {
                ka = (int)whale_get_u16(pc + kp + 1);
                kal = 3;
            } else {
                return 0;
            }
            out->k_is_arg = 1;
            out->k_arg = ka;
            out->k = 0;
            kl = kal;
        }
    }
    {
        int lp = head + gl + kl;

        if (lp >= len)
            return 0;
        if (pc[lp] == OP_lt)
            le = 0;
        else if (pc[lp] == OP_lte)
            le = 1;
        else
            return 0;
        ip = lp + 1;
        if (ip >= len)
            return 0;
        ifop = pc[ip];
        if (ifop == OP_if_false8 && ip + 1 < len)
            exitp = ip + 1 + (int8_t)pc[ip + 1];
        else if (ifop == OP_if_false && ip + 4 < len)
            exitp = ip + 1 + (int32_t)whale_get_u32(pc + ip + 1);
        else
            return 0;
        if (exitp < 0 || exitp > len)
            return 0;
    }
    /* `g = C0` right before the head */
    {
        int vp = -1, vl = 0;

        if (head < 2)
            return 0;
        if (pc[head - 1] >= OP_put_loc0 && pc[head - 1] <= OP_put_loc3)
            vp = pc[head - 1] - OP_put_loc0;
        else if (pc[head - 1] == OP_put_loc8 && head < len)
            vp = (int)pc[head];
        else
            return 0;
        if (vp != g)
            return 0;
        if (!whale_push_lit(pc, len, head - 2, &c0v, &vl) || c0v < 0 ||
            c0v > 2147483647.0 || c0v != (double)(int)c0v)
            return 0;
        /* literal bound: refuse statically-empty trips here; a parameter
           bound is guarded at run time (non-positive length skips the store,
           which is exactly what zero trips do). */
        if (!out->k_is_arg && (le ? c0v > kv : c0v >= kv))
            return 0;
    }
    /* body: the head test's own loads are not the store's - start counting
       after the `if` (ip + its size). Exactly two get_loc (one must read g,
       the other the container), exactly one literal push 0..255, exactly one
       put, and nothing else but shuffling/skeleton. */
    {
        int loc_g = 0, loc_other = 0, lits = 0;
        double litval = 0;
        int body = ip + (ifop == OP_if_false8 ? 2 : 5);

        for (q = body; q <= back; ) {
            int qo = (int)pc[q];

            qsz = whale_op_size_of(qo);
            if (qsz <= 0 || q + qsz > len)
                return 0;
            if (qo == OP_goto8 || qo == OP_goto || qo == OP_if_false8 ||
                qo == OP_if_false || qo == OP_if_true8 || qo == OP_if_true) {
                int t2 = -1;

                if ((qo == OP_goto8 || qo == OP_if_false8 || qo == OP_if_true8) &&
                    q + 1 < len)
                    t2 = q + 1 + (int8_t)pc[q + 1];
                else if (q + 4 < len)
                    t2 = q + 1 + (int32_t)whale_get_u32(pc + q + 1);
                if (t2 < head || t2 > back) {
                    int is_head_test = (q == ip);
                    int is_back_edge = (q == back);

                    if (!((t2 == exitp && is_head_test) || (t2 == head && is_back_edge)))
                        return 0;
                }
                q += qsz;
                continue;
            }
            if (qo == OP_put_array_el) {
                if (puts != 0)
                    return 0;
                puts++;
                q += qsz;
                continue;
            }
            {
                int li = -1, ll = 0;
                double pv = 0;
                int pl = 0;

                if (whale_get_loc_at(pc, len, q, var_count, &li, &ll)) {
                    if (li == g)
                        loc_g++;
                    else
                        loc_other++;
                    q += qsz;
                    continue;
                }
                if (whale_push_lit(pc, len, q, &pv, &pl)) {
                    if (pv < 0 || pv > 255 || pv != (double)(int)pv)
                        return 0;
                    lits++;
                    litval = pv;
                    q += qsz;
                    continue;
                }
            }
            {
                int w = whale_writes_local(qo, pc, q);

                if (w == g) {
                    if (q == back - 2)
                        writes++;
                    else
                        return 0;
                } else if (w >= 0) {
                    return 0;
                }
            }
            /* stack shuffling around the store is fine; calls and anything
               with effects is not */
            switch (qo) {
            case OP_swap: case OP_dup: case OP_nip: case OP_nip1:
            case OP_drop: case OP_to_propkey: case OP_to_propkey2:
            case OP_is_undefined_or_null:
                break;
            default:
                if (!whale_is_pure_num_op(qo))
                    return 0;
                break;
            }
            q += qsz;
        }
        if (writes != 1 || puts != 1 || loc_g != 1 || loc_other != 1 ||
            lits != 1)
            return 0;
        cv = litval;
    }
    {
        int k;

        for (k = 0; k < call_n; k++)
            if (call_pos[k] >= head && call_pos[k] <= back)
                return 0;
    }
    {
        int qq;

        for (qq = head + 1; qq <= back; qq++)
            if (loop_back[qq] >= 0)
                return 0;
    }
    out->g = g;
    out->c0 = c0v;
    out->k = kv;
    out->le = le;
    out->c = cv;
    out->exitp = exitp;
    return 1;
}

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
/* Single-assignment constant locals: lo_c[d] means local d is a compile-time
   constant lo_cv[d] everywhere afterwards. Written by put_loc (fast path and
   generic path), read by the analyser and by local_inline below - a deferred
   local that turns out to be a constant must inline as the literal, not as
   the variable that will only hold it after the flush. */
static uint8_t whale_lo_c[WHALE_JIT_MAX_SLOTS];
static double whale_lo_cv[WHALE_JIT_MAX_SLOTS];
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
/* Whether this function hands an object back. If it does, the objects it builds have to
   be real ones - they leave, and another compilation will meet them. If it does not, the
   frame-column path stays, which needs no allocation and is already fast. Set by the scan
   pass, read when an object literal is compiled. */
static int whale_ret_obj_g;
/* Which line bailed. Every goto out sets it, so the diagnostic at out: can name the exact place instead of making the reader guess from pos/op - the position reported is often a jump target, not the reason. */
static int whale_out_line;
/* An opcode this pass cannot lower does not have to sink the whole function.
   whale_step_emit hands that single instruction to the engine's step path and
   the rest of the function stays native; only if even that refuses does the
   function fall back to interpreting, as before. Control-flow opcodes are
   refused inside whale_step_emit itself, so the hand-over is wired in at every
   bail site instead of each site having to ask first. */
#define WHALE_STEP_INSTEAD_OF_BAILING()                                    \
    do {                                                                   \
        if (whale_step_emit(op, pos, &sp, cur, var_count, stack_size))      \
            goto whale_next_op;                                             \
        whale_out_line = __LINE__;                                          \
        goto out;                                                           \
    } while (0)
/* Whether the current call has an argument that is not a plain number. A self-call
   with one cannot take the self-recursion path, because that path packs its
   arguments into a double array. Set just before the call routes. */
static int whale_call_obj_arg;
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
    whale_vpin_next = 0;
    whale_empty_ranges = 0;
    /* no comma sequence carries across compilations (stale base/len would
       alias the fresh output). */
    whale_comma_open = 0;
    whale_comma_out = NULL;
    whale_comma_base = 0;
    whale_comma_len = 0;

    memset(whale_vname, 0, sizeof(whale_vname));
}

static int fold_mark(void)
{
    return JS_WhaleTokSinkLen();
}

/* A value whose expression grows past this many tokens is materialised right
   away. Without a ceiling, inlining a value used many times (a comparison
   inlined three times, say) could grow the function exponentially.

   A variable rather than a constant, because the right ceiling depends on the
   shape of the function. 24 was picked while measuring pinned registers, where a
   long expression competes with the pinned set for TinyCC's evaluation stack. A
   function with no object work at all - `bitops` is nothing but int32 arithmetic
   - has nothing to compete with, and a ceiling that low there only forces every
   intermediate through memory: with 24 the bitops loop stored and reloaded for
   each of its five operations. */
static int whale_expr_max = 24;

static void fold_materialize(int k);
static void fold_commit(int mark);
/* (whale_comma_close fwd-declared at the top, next to wsrc). */

/* Record [mark, now) as the value of stack slot k, with loop-invariance inv.
   inv is a parameter rather than a post-hoc write-back for one reason: a
   range that grew past the length ceiling is materialised right here, inside
   this call, and the hoist decision reads inv then - a value written back
   afterwards arrives one materialisation too late (measured: the div in
   nested's chain materialised with inv=0 and never hoisted, because the
   binop's AND landed after fold_set returned).

   Only the emitters that can prove invariance pass nonzero (literal pushes,
   get_arg, binop AND, constant/unwritten-local reads). In particular a
   helper/call result must never inherit its arguments' invariance - hoisting
   a call out of a loop repeats the first iteration's value forever. */
static void fold_set(int k, int mark, int inv)
{
    whale_vexpr[k].off = mark;
    whale_vexpr[k].len = JS_WhaleTokSinkLen() - mark;
    whale_vmat[k] = 0;
    whale_vexpr[k].inv = (uint8_t)(inv ? 1 : 0);
    /* overlong expressions take their own statement (a long chain inside a
       comma sequence risks spilling inside TCC's expression evaluator -
       the fused statement would be worse than two plain ones). NB: this
       fires for sub-expressions too (a guard arm's fold_set inside a `%`
       guard) - those must NOT close the sequence (the `;` would land
       mid-expression and unbalance parens). Only statement-level
       materialisations (fold_materialize/local_materialize called directly,
       not from here) join or break sequences; here just materialise. */
    if (whale_vexpr[k].len > whale_expr_max) {
        /* Statements commit eagerly and completely (see fold_materialize),
           so a sub-expression's store cannot disturb any outer sequence:
           the output-tail adjacency test simply fails afterwards and the
           outer store starts a fresh statement. No save/restore needed. */
        fold_materialize(k);
    }
}

/* Loop-invariance of a peephole load operand: literals and parameters are
   constant; a local read is invariant iff the loop body never writes it. */
static int whale_load_inv(int kind, int idx)
{
    if (kind == WLD_INT || kind == WLD_REAL || kind == WLD_ARG)
        return 1;
    if (kind == WLD_SLOT)
        return whale_local_invariant(idx);
    return 0;
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
    if (whale_dbg_env("WHALE_EMPTY_DBG") && !whale_vmat[k] && whale_vexpr[k].len == 0)
        fprintf(stderr, "[empty] slot=%d (len=0, off=%d)\n", k, whale_vexpr[k].off);
    if (whale_vmat[k]) {
        wt_vname(whale_vname[k]);   /* already a real variable */
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
static void fold_toint32(int k, int t)
{
    /* A literal int32 constant needs no conversion at all: emit it as an
       integer literal. Bitwise code is full of these (255, 3, 1, 0xffff). */
    if (t == WHALE_T_ICONST && !whale_vmat[k] && whale_vexpr[k].len == 1) {
        wt_int((int)gw_num[whale_vexpr[k].off]);
        return;
    }
    /* A bare-int expression needs no conversion at all - it already is an
       int. This is the IBIT payoff: chained bitwise ops inline each other
       with zero casts between them. */
    if (t == WHALE_T_IBIT) {
        fold_inline(k);
        return;
    }
    /* A value already known to be an in-range int32 truncates to itself, so
       a single (int) cast is the whole conversion - the (long long) step
       only exists to wrap out-of-range doubles. The C operator needs an int
       operand either way (double & double does not compile), and this keeps
       chained bitwise expressions short enough to stay out of _ws.v[]
       temporaries. */
    if (t == WHALE_T_I32 || t == WHALE_T_ICONST) {
        wt('('); wk(JS_WK_INT); wt(')'); wt('('); fold_inline(k); wt(')');
        return;
    }
    if (whale_type_is_num(t)) {
        wt('('); wk(JS_WK_INT); wt(')'); wt('('); wk(JS_WK_LONG); wk(JS_WK_LONG); wt(')');
        wt('('); fold_inline(k); wt(')');
        return;
    }

    /* Not a number at all - a string, an object, a parked value. ToNumber is the
       engine's job. */
    wt_id("JS_WhaleToInt32"); wt('('); fold_inline(k); wt(')');
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
static int whale_prof_two_shapes(unsigned off);
static unsigned whale_prof_two_off;
/* Emit a deopt-census call: JS_WhaleDeoptWhy(ctx, code) with 0=guard,
   1=shape-miss, 2=tag-miss. Call only inside deopt branches (never steady),
   right next to `*deopt = 1`, so the steady path pays nothing. Guard-class
   exits (entry guards, arg unbox) deliberately emit NO call: they sit right
   before int-region loops, and even one extra call in that branch moves the
   loop alignment enough to cost 30-50% on pure-numeric loops (measured,
   <110>). Guard deopts are derived instead: total - shape - tag. */
static void whale_emit_deopt_why(int code)
{
    wt_id("JS_WhaleDeoptWhy"); wt('('); wt_id("ctx"); wt(','); wt_int(code); wt(')'); wt(';');
}
static void fold_obj_field_load(int os, int atom, int fc, int fc2, int nm,
                                  int stable)
{
    int fm = fold_mark();

    /* `stable` means the shape cannot change for the rest of the function: the
       object cannot escape (no calls) and every store targets a field the
       literal already defined. The resolution then happens once and the shape
       check disappears entirely, which is the difference between beating the
       interpreter and merely matching it.
       Two states, one check: `fv == fver + 1` means this call already ran the
       full path below (resolve + tag normalisation), so the steady state is a
       direct `fp->u.float64` load with no check, no call, no tag dispatch.
       The first trip runs the full path: resolve, null check, value load with
       tag dispatch, and - when the tag was not already 8 - a write-back that
       normalises the field to float64. Past that the tag is provably 8: only
       numbers ever reach the steady state (anything else deopts - the value
       helper above unconditionally does, the store resolve below admits only
       8/0 and every numeric store writes 8), and the shape cannot change
       (that is what `stable` promises). `_wsc` is zeroed per call, so a fresh
       call re-runs the full path; a re-JIT gets a fresh `_wsc` too.
       Single-shape only: when the site saw two shapes (poly-2 below, fc2
       allocated) the same slot alternates identities and the fv mark cannot
       tell them apart - that case falls through to the poly branch. */
    if (stable && fc2 < 0) {
        wk(JS_WK_IF); wt('('); wt_arr("fv", fc); wt_op(JS_OP_NE);
        wt_id("_wsc"); wt('.'); wt_id("fver"); wt('+'); wt_int(1); wt(')'); wt('{');
        wt_arr("fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc); wt(')'); wt(';');
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fp", fc); wt(')'); wt('{');
        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); whale_emit_deopt_why(1); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
        wt_arr("v", nm); wt('=');
        wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
        wt('?'); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
        wt(':'); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
        wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
        wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(','); wt_arr("fp", fc); wt(','); wt_id("deopt"); wt(')');
        wt(';');
        /* normalise: the value just loaded is a number (anything else deopted
           inside the dispatch above), so writing it back as float64 makes the
           tag 8 for every later trip. One extra store on the first trip only. */
        wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt('=');
        wt_arr("v", nm); wt(';');
        wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt('='); wt_int(8); wt(';');
        wt_arr("fv", fc); wt('='); wt_id("_wsc"); wt('.'); wt_id("fver"); wt('+'); wt_int(1); wt(';');
        wt('}');
        wt_arr("v", nm); wt('='); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt(';');
        fold_commit(fm);
        return;
    }
    /* Polymorphic-2 (small polymorphic IC): the warm-up saw exactly two shapes
       at this site, alternating. One cache pair would re-resolve on every trip;
       two pairs (fc/fc2) hold one resolved pointer each. One ObjShape call per
       compare (two at most, short-circuited); only a third shape runs the full
       resolve (parked in the second pair, keeping the first-seen - hottest -
       entry pinned). No shape universe is fenced off: anything unresolvable
       still deopts exactly like the single-pair path below. */
    if (fc2 >= 0 && whale_prof_two_shapes(whale_prof_two_off)) {
        /* if (S==fsA) v=disp(fpA); else if (S==fsB) v=disp(fpB);
           else { fpB=FieldPtr; if (!fpB) deopt; v=disp(fpB); } */
        wk(JS_WK_IF); wt('(');
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')');
        wt_op(JS_OP_EQ); wt_arr("fs", fc); wt(')'); wt('{');
        wt_arr("v", nm); wt('=');
        wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
        wt('?'); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
        wt(':'); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
        wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
        wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(','); wt_arr("fp", fc); wt(','); wt_id("deopt"); wt(')');
        wt(';'); wt('}'); wk(JS_WK_ELSE); wt('{');
        wk(JS_WK_IF); wt('(');
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')');
        wt_op(JS_OP_EQ); wt_arr("fs", fc2); wt(')'); wt('{');
        wt_arr("v", nm); wt('=');
        wt('('); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
        wt('?'); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
        wt(':'); wt('('); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
        wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
        wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(','); wt_arr("fp", fc2); wt(','); wt_id("deopt"); wt(')');
        wt(';'); wt('}'); wk(JS_WK_ELSE); wt('{');
        wt_arr("fp", fc2); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc2); wt(')'); wt(';');
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fp", fc2); wt(')'); wt('{');
        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); whale_emit_deopt_why(1); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
        /* first fill parks in pair A too (it is still empty): afterwards A
           holds the first-seen shape and B the runner-up. */
        wk(JS_WK_IF); wt('('); wt_arr("fs", fc); wt_op(JS_OP_EQ); wt_int(0); wt(')'); wt('{');
        wt_arr("fp", fc); wt('='); wt_arr("fp", fc2); wt(';');
        wt_arr("fs", fc); wt('='); wt_arr("fs", fc2); wt(';');
        wt('}');
        wt_arr("v", nm); wt('=');
        wt('('); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')');
        wt('?'); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
        wt(':'); wt('('); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')');
        wt('?'); wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
        wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(','); wt_arr("fp", fc2); wt(','); wt_id("deopt"); wt(')');
        wt(';'); wt('}'); wt('}');
        fold_commit(fm);
        return;
    }
    wk(JS_WK_IF); wt('('); wt_arr("fv", fc); wt_op(JS_OP_NE); wt_id("_wsc"); wt('.'); wt_id("fver");
    wt_op(JS_OP_OR); wt_arr("fs", fc); wt_op(JS_OP_NE);
    wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
    wt('{');
    wt_arr("fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc); wt(')'); wt(';');
    wt_arr("fv", fc); wt('='); wt_id("_wsc"); wt('.'); wt_id("fver"); wt(';');
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

/* Field-access stability for one access site: the object's shape cannot
   change for the rest of this call, so cached field pointers (VAL/OBJ
   slots) and layout columns (STRUCT) stay valid. Takes the function-level
   proof as a parameter (it lives in whale_jit_gen locals, out of reach
   here): call sites pass `whale_shape_stable || prof(pos)`.
   OBJ slots: the function-level proof suffices. VAL slots - where every
   object local unifies, so a plain IS_OBJ test is dead for locals - need
   additionally that no loop body can swap the slot's identity: no bytecode
   call (which could hand the object out for reshaping; helper calls from
   lowering cannot) and no fresh object or store into an object local in a
   loop. STRUCT columns ride the same proof (columns derive from the shape).
   Field-value stores (put_field) never touch the identity. */
/* Loop-level stability table, filled once per compilation next to the
   shape_stable computation and read at emit time (helper above). NULL
   outside gen; cleared with the table at out:. */
static uint8_t *whale_loop_stable_tab;
/* 1 when the current emit position sits inside a shape-stable loop body.
   Lets a loop go stable even when a pre-loop store kills the function
   level (e.g. `o.r = 0` before the loop: the shape grows once, early). */
static int whale_loop_stable_here(void);
static int whale_field_stable(int ta, int shape_proved)
{
    if (!shape_proved)
        return 0;
    if (WHALE_IS_OBJ(ta))
        return 1;
    if ((WHALE_IS_VAL(ta) || WHALE_IS_STRUCT(ta)) &&
        !whale_loop_has_bcall && !whale_loop_shape_dirty)
        return 1;
    return 0;
}

/* Resolve `atom` of the object in slot `os` through cache pair `fc` without
   loading anything, so a store can follow. Emits a deopt when the value it
   would overwrite is not a number: overwriting a reference without freeing it
   would leak. */
static void fold_obj_field_resolve(int os, int atom, int fc, int fc2,
                                     int stable)
{
    int fm = fold_mark();

    /* Steady-state twin of the load above: `fv == fver + 1` means resolved
       and tag-normalised, so a store needs no check at all - the call site's
       `fp->u.float64 = <value>` runs unconditionally (the tag is already 8
       and stays 8: only numbers are stored on this path). The first trip runs
       the full resolve plus the numeric-tag admission (8 or 0, else deopt -
       overwriting a reference without freeing it would leak). Single-shape
       only (see the load above): a poly-2 site falls through to its branch. */
    if (stable && fc2 < 0) {
        wk(JS_WK_IF); wt('('); wt_arr("fv", fc); wt_op(JS_OP_NE);
        wt_id("_wsc"); wt('.'); wt_id("fver"); wt('+'); wt_int(1); wt(')'); wt('{');
        wt_arr("fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc); wt(')'); wt(';');
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fp", fc); wt(')'); wt('{');
        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); whale_emit_deopt_why(1); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
        wk(JS_WK_IF); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(8); wt(')');
        wt('{'); wk(JS_WK_IF); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(0); wt(')');
        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}'); wt('}');
        wt_arr("fv", fc); wt('='); wt_id("_wsc"); wt('.'); wt_id("fver"); wt('+'); wt_int(1); wt(';');
        wt('}');
        fold_commit(fm);
        return;
    }
    /* Poly-2 store-side twin of the load above: pick the resolved pointer by
       shape, resolving into the second pair on a double miss. The call site's
       value store follows unconditionally in all three arms (it names fp[fc]
       after we normalise the choice into it). */
    if (fc2 >= 0 && whale_prof_two_shapes(whale_prof_two_off)) {
        wk(JS_WK_IF); wt('(');
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')');
        wt_op(JS_OP_NE); wt_arr("fs", fc); wt(')'); wt('{');
        wk(JS_WK_IF); wt('(');
        wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')');
        wt_op(JS_OP_EQ); wt_arr("fs", fc2); wt(')'); wt('{');
        wt_arr("fp", fc); wt('='); wt_arr("fp", fc2); wt(';');
        wt('}'); wk(JS_WK_ELSE); wt('{');
        wt_arr("fp", fc2); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
        wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc2); wt(')'); wt(';');
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fp", fc2); wt(')'); wt('{');
        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); whale_emit_deopt_why(1); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
        wk(JS_WK_IF); wt('('); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(8); wt(')');
        wt('{'); wk(JS_WK_IF); wt('('); wt_arr("fp", fc2); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(0); wt(')');
        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); whale_emit_deopt_why(2); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}'); wt('}');
        /* first fill parks in pair A too (it is still empty): afterwards A
           holds the first-seen shape and B the runner-up. */
        wk(JS_WK_IF); wt('('); wt_arr("fs", fc); wt_op(JS_OP_EQ); wt_int(0); wt(')'); wt('{');
        wt_arr("fp", fc); wt('='); wt_arr("fp", fc2); wt(';');
        wt_arr("fs", fc); wt('='); wt_arr("fs", fc2); wt(';');
        wt('}'); wk(JS_WK_ELSE); wt('{');
        wt_arr("fp", fc); wt('='); wt_arr("fp", fc2); wt(';');
        wt('}');
        wt('}'); wt('}');
        fold_commit(fm);
        return;
    }
    wk(JS_WK_IF); wt('('); wt_arr("fv", fc); wt_op(JS_OP_NE); wt_id("_wsc"); wt('.'); wt_id("fver");
    wt_op(JS_OP_OR); wt_arr("fs", fc); wt_op(JS_OP_NE);
    wt_id("JS_WhaleObjShape"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
    wt('{');
    wt_arr("fp", fc); wt('='); wt_id("JS_WhaleObjFieldPtr"); wt('(');
    wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(atom); wt(','); wt('&'); wt_arr("fs", fc); wt(')'); wt(';');
    wt_arr("fv", fc); wt('='); wt_id("_wsc"); wt('.'); wt_id("fver"); wt(';');
    wk(JS_WK_IF); wt('('); wt('!'); wt_arr("fp", fc); wt(')'); wt('{');
    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    wt('}');
    wk(JS_WK_IF); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(8); wt(')');
    wt('{'); wk(JS_WK_IF); wt('('); wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(0); wt(')');
    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); whale_emit_deopt_why(2); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}'); wt('}');
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

/* Comma-fused stores (notes <116>): consecutive pure stores
   (`L5=..; L4=..; L3=..`, no control flow / call / aliasing between them)
   fuse into one comma expression (`L5=.., L4=.., L3=..;`) - AST level, no
   new IR, tokens only. TCC treats it as one statement (one spill boundary
   instead of three).

   Buffering, not sink marks: a pending segment is committed to the output
   eagerly with its `;`, and continuation rewrites that tail. An open sequence
   is exactly one committed statement in the current output buffer whose tail
   is `... <expr> ;` - continuing pops the `;`, pushes `, <next-store> ;`.
   Helper-call statements between stores break the chain automatically (the
   output tail is no longer ours to rewrite).

   This replaced a sink-mark scheme (adjacent-in-sink meant continuation) that
   double-emitted: the eager commit that founded the sequence and the close
   that recommitted it overlapped (writeorder: `_wsv[0]=(E) [0]=(E);`). */
/* (state fwd-declared at the top, next to wsrc.) */
/* Close the open sequence, if any: it is already committed (with its `;`),
   so closing only clears the state. Called at every basic-block boundary
   (flush/jump/return/loop-buffer switch), before truncates, and at
   function end. */
static void whale_comma_close(void)
{
    whale_comma_open = 0;
}

/* Try to continue the open comma sequence: the current output must still end
   with our statement (`;` tail, same buffer, nothing appended since). Pops
   the `;`, pushes `,` - the caller then emits `<store> ;` into the sink and
   commits, so the fused statement is one commit, never two overlapping ones.
   Returns 1 on continuation, 0 for a fresh statement (caller closes first). */
static int whale_comma_continue(void)
{
    int *lenp = whale_out_len;

    if (!whale_comma_open || whale_comma_out != whale_out_tok)
        return 0;
    if (*lenp != whale_comma_base + whale_comma_len)
        return 0;
    if (whale_comma_len < 1 || whale_out_tok[*lenp - 1] != ';')
        return 0;
    /* pop the `;` (numbers travel alongside, keep them aligned) */
    (*lenp)--;
    wt(',');
    return 1;
}

/* WhaleJS: the loop buffer is ON.
 *
 * It hands a complete loop body to a loop-level pass, and that pass now exists -
 * this is where the native `for`/`while` gets built, from the loop table the
 * compiler produced (JSWhaleFuncInfo.loops) rather than from the goto structure.
 *
 * Two things had to be fixed before it could be switched back on: WHALE_LOOP_BUF
 * was 4096 while every writer bounds itself by WHALE_TOK_MAX, so a longer body ran
 * past the end of l->tok; the buffer is now WHALE_TOK_MAX, which is safe by
 * construction. The returned 0 path is kept for a real overflow - too deep, or the
 * buffer is somehow full - and then the body is emitted straight into go_tok, which
 * is the bound the whole function is held to anyway. */
static int loop_begin(int head_pos, int back_pos)
{
    WhaleLoop *l;

    /* a comma sequence never crosses a buffer switch. */
    whale_comma_close();
    if (whale_loop_depth >= WHALE_LOOP_MAX)
        return 0;                       /* too deep: emit straight through */
    l = &whale_loops[whale_loop_depth];
    l->len = 0;
    l->head_pos = head_pos;
    l->back_pos = back_pos;
    whale_loop_depth++;
    whale_out_tok = l->tok;
    whale_out_num = l->num;
    whale_out_len = &l->len;
    return 1;
}

/* Finish the innermost loop and hand its body on. This is where a loop-level
   optimisation would run - it gets the complete body at once. */
static int whale_loop_unroll(WhaleLoop *l, int trip, int g, int exitp);
static int whale_loop_memset_emit(WhaleLoop *l, const WhaleMemsetLoop *ms);
static int whale_loop_count_emit(WhaleLoop *l, const WhaleCountLoop *cs);
static void local_materialize(int d);
static void loop_end(void)
{
    WhaleLoop *l, *p;
    int i, n;

    /* same reason as loop_begin: no sequence crosses the switch. */
    whale_comma_close();
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
    /* Small-trip-count unrolling: the body is complete in l->tok, so a proven
       `for (g = 0; g < K; g++)` (K <= 8) is expanded inline here instead of
       being emitted as a loop. Anything unexpected falls through to the
       plain append below - unrolling is best-effort, never load-bearing. */
    if (whale_loop_trip && l->head_pos >= 0 &&
        whale_loop_unroll(l, whale_loop_trip[l->head_pos],
                          whale_loop_trip_g[l->head_pos],
                          whale_loop_trip_exit[l->head_pos]))
        return;
    /* Memset loop: `for (g = C0; g <= K; g++) arr[g] = C` over a Uint8Array
       becomes one memset call. The TAU8 slot is confirmed from the body
       itself (the base-pointer resolve + the byte store), never from the
       typeless pass-1 scan. */
    if (whale_loop_memset && l->head_pos >= 0 &&
        whale_loop_memset_emit(l, &whale_loop_memset[l->head_pos]))
        return;
    /* Count loop: `for (g = C0; g <= K; g++) if (arr[g]) acc++` over a
       Uint8Array becomes one nonzero-count call. Same confirmation rule. */
    if (whale_loop_countrec && l->head_pos >= 0 &&
        whale_loop_count_emit(l, &whale_loop_countrec[l->head_pos]))
        return;
    n = l->len;
    if (n > WHALE_LOOP_BUF)
        n = WHALE_LOOP_BUF;
    for (i = 0; i < n && *whale_out_len < WHALE_TOK_MAX; i++) {
        whale_out_num[*whale_out_len] = l->num[i];
        whale_out_tok[(*whale_out_len)++] = l->tok[i];
    }
}

/* Expand the completed body of a proven small loop inline: `for (g = 0;
   g < K; g++) body` becomes K copies of body with g spelled as 0..K-1.
   Returns 1 when the body was consumed (loop_end appends nothing more).

   Buffer shape (emitted in order): `Lhead:`, head flush stores, the head
   test branch `if (..) goto Lexit;`, the body, the back-edge `goto Lhead;`.
   The expansion keeps the head flush once, drops the head label, the test
   and the back edge, deletes the induction increment `Lg = (Lg+1);`, and
   renames every other inner label (`Ln:` / `goto Ln`) per copy so copies do
   not collide. The exit edge vanishes with the test: every copy runs, which
   is exactly what K trips do. Any shape surprise bails to 0 and the loop is
   emitted as before. */
static int whale_loop_unroll(WhaleLoop *l, int trip, int g, int exitp)
{
    int head, n, i;
    int headid, exitid, lgid;
    int cond_end, body_start, body_end, back_start;
    char lgname[32];

    if (trip <= 1 || trip > 8 || g < 0 || exitp < 0)
        return 0;
    head = l->head_pos;
    if (head < 0)
        return 0;
    n = l->len;
    if (n <= 0 || n > WHALE_LOOP_BUF)
        return 0;
    snprintf(lgname, sizeof(lgname), "L%d", head);
    headid = JS_WhaleTokIdent(lgname);
    snprintf(lgname, sizeof(lgname), "L%d", exitp);
    exitid = JS_WhaleTokIdent(lgname);
    snprintf(lgname, sizeof(lgname), "L%d", g);
    lgid = JS_WhaleTokIdent(lgname);
    /* [0] must be the head label `Lhead:` */
    if (n < 2 || l->tok[0] != headid || l->tok[1] != ':') {
        return 0;
    }
    /* first `if` after the label ends the head part (flush stores + test) */
    cond_end = -1;
    for (i = 2; i < n; i++) {
        if (JS_WhaleTokIsKw(l->tok[i], JS_WK_IF)) {
            int j = i;

            while (j < n && l->tok[j] != ';')
                j++;
            if (j >= n)
                return 0;
            cond_end = j + 1;
            break;
        }
    }
    if (cond_end < 0)
        return 0;
    body_start = cond_end;
    /* trailing `goto Lhead;` is the back edge: exactly the last statement */
    back_start = -1;
    if (n - 3 >= body_start && l->tok[n - 1] == ';' &&
        l->tok[n - 2] == headid &&
        JS_WhaleTokIsKw(l->tok[n - 3], JS_WK_GOTO))
        back_start = n - 3;
    if (back_start < 0)
        return 0;
    body_end = back_start;
    /* Compile-time guard: trip is capped at 8, but a huge body times 8 is a
       huge function (compile time + icache). Cap the single body; anything
       bigger stays a loop. 2048 tokens is ~10x minmax's inner body. */
    if (body_end - body_start > 2048)
        return 0;
    {
        int k, j;

        /* inner labels first: every `Ln:` in the body (other than head and
           exit) is collected once, so both its definition and any forward
           `goto Ln` before it map to the same per-copy name. */
        int lab_old[32];
        int nlab = 0;

        for (j = body_start; j + 1 < body_end; j++) {
            if (l->tok[j + 1] == ':' && l->tok[j] != headid &&
                l->tok[j] != exitid && l->tok[j] != lgid) {
                int m, found = 0;

                for (m = 0; m < nlab; m++)
                    if (lab_old[m] == l->tok[j]) {
                        found = 1;
                        break;
                    }
                if (!found && nlab < 32)
                    lab_old[nlab++] = l->tok[j];
            }
        }
        /* `goto Lexit` must not appear (only the head test may leave, and it
           is deleted with the test); any other `goto Ln` must name a
           collected inner label. Otherwise the body has control flow this
           expander does not understand. */
        for (j = body_start; j + 2 < body_end; j++) {
            if (JS_WhaleTokIsKw(l->tok[j], JS_WK_GOTO) &&
                l->tok[j + 2] == ';') {
                int t = l->tok[j + 1];
                int m, ok = 0;

                if (t == exitid)
                    return 0;
                for (m = 0; m < nlab; m++)
                    if (lab_old[m] == t) {
                        ok = 1;
                        break;
                    }
                if (!ok)
                    return 0;
            }
        }
        /* head flush stores ([2, if_start)) go out once: they carry the
           values from before the loop. The head test itself (`if ... goto
           exit;`) dies with the loop (every copy runs). Find where the `if`
           starts so copies begin after it. */
        {
            int if_start = -1;

            for (if_start = 2; if_start < body_start; if_start++)
                if (JS_WhaleTokIsKw(l->tok[if_start], JS_WK_IF))
                    break;
            if (if_start >= body_start)
                return 0;
            for (j = 2; j < if_start && *whale_out_len < WHALE_TOK_MAX; j++) {
                whale_out_num[*whale_out_len] = l->num[j];
                whale_out_tok[(*whale_out_len)++] = l->tok[j];
            }
        }
        for (k = 0; k < trip; k++) {
            int lab_new[32];
            int m;

            for (m = 0; m < nlab; m++) {
                char nb[48];

                snprintf(nb, sizeof(nb), "Q%x_%d", (unsigned)lab_old[m], k);
                lab_new[m] = JS_WhaleTokIdent(nb);
            }
            for (j = body_start; j < body_end; j++) {
                int t = l->tok[j];
                double v = l->num[j];

                if (t == lgid) {
                    /* induction increment `Lg = ...;` dies (g is the literal
                       k in this copy); any other store to Lg would have
                       refused the trip, so `Lg =` always opens it. */
                    if (j + 1 < body_end && l->tok[j + 1] == '=') {
                        while (j < body_end && l->tok[j] != ';')
                            j++;
                        continue;
                    }
                    /* a read of the induction variable: the trip literal */
                    if (*whale_out_len < WHALE_TOK_MAX) {
                        whale_out_num[*whale_out_len] = (double)k;
                        whale_out_tok[(*whale_out_len)++] =
                            JS_WhaleTokCInt();
                    }
                    continue;
                }
                /* a collected inner label (definition or goto target) */
                {
                    int found = -1;

                    for (m = 0; m < nlab; m++)
                        if (lab_old[m] == t) {
                            found = m;
                            break;
                        }
                    if (found >= 0) {
                        if (*whale_out_len < WHALE_TOK_MAX) {
                            whale_out_num[*whale_out_len] = v;
                            whale_out_tok[(*whale_out_len)++] =
                                lab_new[found];
                        }
                        continue;
                    }
                }
                if (*whale_out_len < WHALE_TOK_MAX) {
                    whale_out_num[*whale_out_len] = v;
                    whale_out_tok[(*whale_out_len)++] = t;
                }
            }
        }
    }
    return 1;
}

/* Replace `for (g = C0; g <= K / g < K; g++) arr[g] = C` over a Uint8Array
   with one memset call. Returns 1 when the body was consumed.

   The TAU8 slot is confirmed from the body tokens themselves: exactly one
   `JS_WhaleTAU8Ptr(ctx, os, &on[os])` resolve and exactly one byte store
   through `_ws.op[os]`, both naming the same slot, and no other helper call
   or store in the body. The pass-1 scan only proved the loop skeleton
   (`ms`); the array's identity comes from here, where the slot numbers are
   real. Anything unexpected bails to 0 and the loop is emitted as before.

   Emitted shape (all run once, in order): the base resolve (copied from the
   body, so the cache protocol is unchanged), a length guard
   (`K+1 > len` deopts - memset must not run past the buffer), the memset
   itself over `[C0, K]`, and the induction variable's exit value. Types are
   uniform by construction (entry unboxing + deopt on any non-number), so no
   per-element guard is needed - that is what the signature-versioned
   compilation buys. */
static int whale_loop_memset_emit(WhaleLoop *l, const WhaleMemsetLoop *ms)
{
    int n, i, os = -1, stores = 0, calls = 0;
    int tauptr_id, ws_id, op_id, oi_id, on_id;
    int head;

    if (!ms || ms->g < 0 || ms->exitp < 0)
        return 0;
    if (ms->c < 0 || ms->c > 255 || ms->c != (double)(int)ms->c)
        return 0;
    if (!ms->k_is_arg) {
        double trips = ms->le ? ms->k - ms->c0 + 1 : ms->k - ms->c0;

        if (trips < 1 || trips > 2147483647.0)
            return 0;
    }
    /* Compile-time guard: memset length is bounded by the array, but refuse
       absurd trips outright (a hostile literal bound must not hang the
       compiler in the length arithmetic). */
    if (!ms->k_is_arg && ms->k > 1073741824.0)
        return 0;
    head = l->head_pos;
    if (head < 0)
        return 0;
    n = l->len;
    if (n <= 0 || n > WHALE_LOOP_BUF)
        return 0;
    tauptr_id = JS_WhaleTokIdent("JS_WhaleTAU8Ptr");
    ws_id = JS_WhaleTokIdent("_wsc");
    op_id = JS_WhaleTokIdent("op");
    oi_id = JS_WhaleTokIdent("oi");
    on_id = JS_WhaleTokIdent("on");
    /* one resolve, one byte store through the same slot, nothing else that
       calls out or stores */
    for (i = 0; i < n; i++) {
        int t = l->tok[i];

        if (t == tauptr_id) {
            /* `JS_WhaleTAU8Ptr(ctx, os, &on[os])`: os is the CINT after ctx */
            int j = i + 1, depth = 0, slot = -1;

            if (j >= n || l->tok[j] != '(')
                return 0;
            j++;
            /* skip `ctx` */
            while (j < n && l->tok[j] != ',' && l->tok[j] != ')')
                j++;
            if (j >= n || l->tok[j] != ',')
                return 0;
            j++;
            if (j < n && l->tok[j] == JS_WhaleTokCInt()) {
                slot = (int)l->num[j];
                j++;
            } else {
                return 0;
            }
            if (slot < 0 || slot >= WHALE_OBJ_SLOTS)
                return 0;
            if (os < 0)
                os = slot;
            else if (os != slot)
                return 0;
            calls++;
            continue;
        }
        /* byte store `_ws.op[os][..] = ..`: `_ws . op [ os ] [`. The base
           resolve (`_ws.op[os] = ..`, single bracket) must not match - demand
           the second `[`. */
        if (t == ws_id && i + 6 < n && l->tok[i + 1] == '.' &&
            l->tok[i + 2] == op_id && l->tok[i + 3] == '[' &&
            l->tok[i + 4] == JS_WhaleTokCInt() && l->tok[i + 5] == ']' &&
            l->tok[i + 6] == '[') {
            int s2 = (int)l->num[i + 4];

            if (s2 < 0 || s2 >= WHALE_OBJ_SLOTS)
                return 0;
            if (os < 0)
                os = s2;
            else if (os != s2)
                return 0;
            stores++;
            continue;
        }
        /* any other call-looking `ident (` is foreign: an identifier
           directly followed by `(`. Punctuation and single-char operators
           live below 256 and never match; keywords are excluded by table;
           what remains is a real ident call, and the only one this shape
           allows is the resolve above. */
        if (t >= 0 && i + 1 < n && l->tok[i + 1] == '(') {
            int w, iskw = 0;

            if (t == tauptr_id)
                continue;
            if (t < 256)
                continue;
            for (w = 0; w < 28; w++) {
                if (JS_WhaleTokIsKw(t, w)) {
                    iskw = 1;
                    break;
                }
            }
            if (iskw)
                continue;
            return 0;
        }
    }
    if (os < 0 || stores != 1 || calls < 1)
        return 0;
    (void)oi_id;
    (void)on_id;
    {
        int fm = fold_mark();
        char lgname[32];

        /* base resolve, copied from the body's own protocol */
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("oi", os); wt(')'); wt('{');
        wt_arr("op", os); wt('='); wt('('); wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt(')');
        wt_id("JS_WhaleTAU8Ptr"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_arr("on", os); wt(')'); wt(';');
        wt_arr("oi", os); wt('='); wt_int(1); wt(';'); wt('}');
        /* length guard: the bound must fit the buffer (`<=K` needs K+1 <=
           len, `<K` needs K <= len). A parameter bound is read from An[]. */
        wk(JS_WK_IF); wt('('); wt('(');
        if (ms->k_is_arg) {
            wt_id("An"); wt('['); wt_int(ms->k_arg); wt(']');
        } else {
            wt_num(ms->k);
        }
        if (ms->le)
            wt('+'); wt_int(1);
        wt(')'); wt_op(JS_OP_GT); wt_arr("on", os); wt(')');
        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
        /* zero/negative length skips the store (exactly what zero trips do);
           the exit value below still runs. */
        wk(JS_WK_IF); wt('('); wt('(');
        if (ms->k_is_arg) {
            wt_id("An"); wt('['); wt_int(ms->k_arg); wt(']');
        } else {
            wt_num(ms->k);
        }
        wt('-'); wt_num(ms->c0); wt('+'); wt_int(ms->le ? 1 : 0); wt(')');
        wt_op(JS_OP_GT); wt_int(0); wt(')'); wt('{');
        /* the memset itself over [C0, K] (`<=` stores K-C0+1 bytes,
           `<` stores K-C0) */
        wt_id("memset"); wt('('); wt_arr("op", os); wt('+'); wt_int((int)ms->c0); wt(',');
        wt_int((int)ms->c); wt(','); wt('('); wk(JS_WK_UNSIGNED); wk(JS_WK_LONG); wk(JS_WK_LONG); wt(')');
        wt('(');
        if (ms->k_is_arg) {
            wt_id("An"); wt('['); wt_int(ms->k_arg); wt(']');
        } else {
            wt_num(ms->k);
        }
        wt('-'); wt_num(ms->c0); wt('+'); wt_int(ms->le ? 1 : 0); wt(')'); wt(')');
        wt(';');
        wt('}');
        /* induction exit value (`<=` leaves g at K+1, `<` at K) */
        snprintf(lgname, sizeof(lgname), "L%d", ms->g);
        wt_id(lgname); wt('=');
        if (ms->le) {
            wt('(');
            if (ms->k_is_arg) {
                wt_id("An"); wt('['); wt_int(ms->k_arg); wt(']');
            } else {
                wt_num(ms->k);
            }
            wt('+'); wt_int(1); wt(')');
        } else {
            if (ms->k_is_arg) {
                wt_id("An"); wt('['); wt_int(ms->k_arg); wt(']');
            } else {
                wt_num(ms->k);
            }
        }
        wt(';');
        fold_commit(fm);
    }
    return 1;
}

/* Replace `for (g = C0; g <= K; g++) if (arr[g]) acc++` over a Uint8Array
   with one nonzero-count call. Returns 1 when the body was consumed.

   Confirmation mirrors the memset emitter: exactly one `JS_WhaleTAU8Ptr`
   resolve and one `_ws.op[os][..]` read through the same slot, no other
   helper call. (Read or write share the token shape; which one it is was
   decided by the pass-1 scan.) The accumulator is materialised first (its
   pending increment would otherwise never be stored), then `acc = acc +
   CountNZ(slot, C0, K)`. A zero/negative trip skips the call and only sets
   the exit values - exactly what zero trips do. */
static int whale_loop_count_emit(WhaleLoop *l, const WhaleCountLoop *cs)
{
    int n, i, os = -1, reads = 0, calls = 0;
    int tauptr_id, ws_id, op_id, oi_id, on_id;
    int head;

    if (!cs || !cs->ok || cs->g < 0 || cs->acc < 0 || cs->exitp < 0)
        return 0;
    head = l->head_pos;
    if (head < 0)
        return 0;
    n = l->len;
    if (n <= 0 || n > WHALE_LOOP_BUF)
        return 0;
    tauptr_id = JS_WhaleTokIdent("JS_WhaleTAU8Ptr");
    ws_id = JS_WhaleTokIdent("_wsc");
    op_id = JS_WhaleTokIdent("op");
    oi_id = JS_WhaleTokIdent("oi");
    on_id = JS_WhaleTokIdent("on");
    for (i = 0; i < n; i++) {
        int t = l->tok[i];

        if (t == tauptr_id) {
            int j = i + 1, slot = -1;

            if (j >= n || l->tok[j] != '(')
                return 0;
            j++;
            while (j < n && l->tok[j] != ',' && l->tok[j] != ')')
                j++;
            if (j >= n || l->tok[j] != ',')
                return 0;
            j++;
            if (j < n && l->tok[j] == JS_WhaleTokCInt()) {
                slot = (int)l->num[j];
                j++;
            } else {
                return 0;
            }
            if (slot < 0 || slot >= WHALE_OBJ_SLOTS)
                return 0;
            if (os < 0)
                os = slot;
            else if (os != slot)
                return 0;
            calls++;
            continue;
        }
        if (t == ws_id && i + 6 < n && l->tok[i + 1] == '.' &&
            l->tok[i + 2] == op_id && l->tok[i + 3] == '[' &&
            l->tok[i + 4] == JS_WhaleTokCInt() && l->tok[i + 5] == ']' &&
            l->tok[i + 6] == '[') {
            int s2 = (int)l->num[i + 4];

            if (s2 < 0 || s2 >= WHALE_OBJ_SLOTS)
                return 0;
            if (os < 0)
                os = s2;
            else if (os != s2)
                return 0;
            reads++;
            continue;
        }
        if (t >= 0 && i + 1 < n && l->tok[i + 1] == '(') {
            int w, iskw = 0;

            if (t == tauptr_id)
                continue;
            if (t < 256)
                continue;
            for (w = 0; w < 28; w++) {
                if (JS_WhaleTokIsKw(t, w)) {
                    iskw = 1;
                    break;
                }
            }
            if (iskw)
                continue;
            return 0;
        }
    }
    if (os < 0 || reads < 1 || calls < 1)
        return 0;
    (void)oi_id;
    (void)on_id;
    {
        int fm = fold_mark();
        char lgname[32];
        char laname[32];

        local_materialize(cs->acc);
        snprintf(laname, sizeof(laname), "L%d", cs->acc);
        /* base resolve, copied from the body's own protocol */
        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("oi", os); wt(')'); wt('{');
        wt_arr("op", os); wt('='); wt('('); wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt(')');
        wt_id("JS_WhaleTAU8Ptr"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt('&'); wt_arr("on", os); wt(')'); wt(';');
        wt_arr("oi", os); wt('='); wt_int(1); wt(';'); wt('}');
        /* length guard: the bound must fit the buffer */
        wk(JS_WK_IF); wt('('); wt('(');
        if (cs->k_is_arg) {
            wt_id("An"); wt('['); wt_int(cs->k_arg); wt(']');
        } else {
            wt_num(cs->k);
        }
        if (cs->le)
            wt('+'); wt_int(1);
        wt(')'); wt_op(JS_OP_GT); wt_arr("on", os); wt(')');
        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
        /* zero/negative trip skips the call; the exit values still run */
        wk(JS_WK_IF); wt('('); wt('(');
        if (cs->k_is_arg) {
            wt_id("An"); wt('['); wt_int(cs->k_arg); wt(']');
        } else {
            wt_num(cs->k);
        }
        wt('-'); wt_num(cs->c0); wt('+'); wt_int(cs->le ? 1 : 0); wt(')');
        wt_op(JS_OP_GT); wt_int(0); wt(')'); wt('{');
        /* acc = acc + CountNZ(slot, C0, K) */
        wt_id(laname); wt('='); wt_id(laname); wt('+');
        wt_id("JS_WhaleTAU8CountNZ"); wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(',');
        wt_num(cs->c0); wt(',');
        if (cs->k_is_arg) {
            wt_id("An"); wt('['); wt_int(cs->k_arg); wt(']');
        } else {
            wt_num(cs->k);
        }
        wt(')'); wt(';');
        wt('}');
        /* induction exit value */
        snprintf(lgname, sizeof(lgname), "L%d", cs->g);
        wt_id(lgname); wt('=');
        if (cs->le) {
            wt('(');
            if (cs->k_is_arg) {
                wt_id("An"); wt('['); wt_int(cs->k_arg); wt(']');
            } else {
                wt_num(cs->k);
            }
            wt('+'); wt_int(1); wt(')');
        } else {
            if (cs->k_is_arg) {
                wt_id("An"); wt('['); wt_int(cs->k_arg); wt(']');
            } else {
                wt_num(cs->k);
            }
        }
        wt(';');
        fold_commit(fm);
    }
    return 1;
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
       so this cannot run out for any function seen so far.
       ELS: inside a single call-free loop the name is a pinned register local
       (_wv<n>) rather than a state-block slot - the value is recomputed per
       trip and re-read, so keeping it in xmm pays every iteration. Outside
       that shape the block slot is fine (written once, read rarely; nested
       loops fight over the 4-deep pool). Conditions match the prologue
       declaration gate exactly (including loop_count == 1: without it a
       nested loop's outer remainder would name an undeclared _wv after an
       inner loop_end() dropped the depth back to 1). */
    if (!whale_loop_has_call && whale_loop_depth == 1 &&
        whale_emit_loop_count == 1 && whale_vpin_next < WHALE_VPIN_MAX)
        whale_vname[k] = WHALE_VPIN_BASE + whale_vpin_next++;
    else
        whale_vname[k] = whale_vserial++;
    mark = fold_mark();
    /* comma-fused via output-tail rewrite (see above): continuing pops the
       open statement's `;` and joins with `,`; otherwise close first and
       start a fresh statement. WHALE_NO_COMMA restores the old
       immediate-`;` behaviour exactly (each store its own statement). */
    if (getenv("WHALE_NO_COMMA")) {
        whale_comma_close();
        wt_vname(whale_vname[k]); wt('=');
        fold_inline(k);
        wt(';');
        fold_commit(mark);
        whale_vmat[k] = 1;
        return;
    }
    int founded;

    if (!whale_comma_continue()) {
        whale_comma_close();
        whale_comma_out = whale_out_tok;
        whale_comma_base = *whale_out_len;
        founded = 1;
    } else {
        founded = 0;
    }
    wt_vname(whale_vname[k]); wt('=');
    fold_inline(k);
    wt(';');
    fold_commit(mark);
    /* the open statement now spans [base, out_len): continuation rewrites it. */
    whale_comma_open = 1;
    whale_comma_len = *whale_out_len - whale_comma_base;
    /* Loop-invariant hoisting: the value is the same on every iteration, so
       compute it once before the loop instead of on every trip. The statement
       goes to the enclosing output (which sits right before the loop-head
       label - the label itself lives inside the body buffer), and later reads
       inline the variable as usual. Only stack temporaries (_ws.v<n>): locals
       are loop-carried by definition here, and the AND in the binop emitters
       already refused anything with a variable leaf. */
    if (whale_loop_depth > 0 && whale_vexpr[k].inv) {
        int *save_tok = whale_out_tok;
        double *save_num = whale_out_num;
        int *save_len = whale_out_len;
        int hlen = *whale_out_len - whale_comma_base;
        int i;

        /* hoisted statements leave the comma sequence: the store is already
           committed above, so move its output tokens to the enclosing output
           (which sits right before the loop-head label) instead of
           re-committing from the sink (that re-emitted the open segment).
           Only a statement this store founded travels: a continued store may
           join an older chain whose earlier segments are variant, and moving
           those out of the loop would repeat the first trip's values. */
        if (!founded) {
            /* joined an older statement: leave the chain in place. */
            whale_vmat[k] = 1;
            return;
        }
        whale_comma_close();

        if (whale_loop_depth - 1 == 0) {
            whale_out_tok = go_tok;
            whale_out_num = go_num;
            whale_out_len = &go_len;
        } else {
            WhaleLoop *outer = &whale_loops[whale_loop_depth - 2];

            whale_out_tok = outer->tok;
            whale_out_num = outer->num;
            whale_out_len = &outer->len;
        }
        for (i = 0; i < hlen && *whale_out_len < WHALE_TOK_MAX; i++) {
            whale_out_num[*whale_out_len] = save_num[whale_comma_base + i];
            whale_out_tok[(*whale_out_len)++] = save_tok[whale_comma_base + i];
        }
        *save_len = whale_comma_base;
        whale_out_tok = save_tok;
        whale_out_num = save_num;
        whale_out_len = save_len;
    }
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
    if (whale_dbg_env("WHALE_MEM_TRACE"))
        fprintf(stderr, "[mat] L%d off=%d len=%d\n", d, m->expr.off, m->expr.len);
    mark = fold_mark();
    /* comma-fused like fold_materialize above (same tail-rewrite rule). */
    if (!getenv("WHALE_NO_COMMA") && whale_comma_continue()) {
        /* joined: `;` popped, `,` pushed - emit `<store> ;` below */
    } else {
        whale_comma_close();
        whale_comma_out = whale_out_tok;
        whale_comma_base = *whale_out_len;
    }
    /* Int-region mirror: inside the loop the pending expression is an int
       expression (mirror stores wrap `(int)`), so it goes to I<d>, not L<d>.
       Storing it to L<d> would leave later in-loop reads loading a stale
       mirror. Outside the loop the exit edge has boxed the mirror already. */
    if (whale_int_ok && d >= 0 && d < WHALE_JIT_MAX_SLOTS &&
        whale_int_mirror[d] && whale_loop_depth > 0)
        wt_slot("I", d);
    else
        wt_slot("L", d);
    wt('=');
    fold_inline_expr(&m->expr);
    wt(';');
    fold_commit(mark);
    whale_comma_open = 1;
    whale_comma_len = *whale_out_len - whale_comma_base;
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
    if (whale_dbg_env("WHALE_MEM_TRACE"))
        fprintf(stderr, "[inl] L%d live=%d\n", d, whale_lmem[d].live);
    if (!whale_lmem[d].live) {
        if (d >= 0 && d < WHALE_JIT_MAX_SLOTS && whale_lo_c[d]) {
            wt_num(whale_lo_cv[d]);
            return;
        }
        /* Int-region mirror: inside the loop the running value is I<d>.
           (A pending expression inlines as-is either way - mirror stores
           already wrap `(int)`.) */
        if (whale_int_ok && d >= 0 && d < WHALE_JIT_MAX_SLOTS &&
            whale_int_mirror[d] && whale_loop_depth > 0) {
            wt_slot("I", d);
            return;
        }
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

/* Whether the pending expression of local d names the variable L<d> itself.
   A self-copy (`L0 = (L0)`, a parser-folded `s * 1`) must not stay deferred:
   every later read then inlines `(L0)` instead of loading the variable, and
   measured on ident that is pure loss (86 -> 120ms+). A `_wv[n]` copy names
   no local, so the short-expression exemption below still applies to it. */
static int local_self_ref(int d)
{
    char lb[32];
    int lid, i;
    WhaleLocal *m = &whale_lmem[d];

    snprintf(lb, sizeof(lb), "L%d", d);
    lid = JS_WhaleTokIdent(lb);
    for (i = 0; i < m->expr.len; i++)
        if (gw_tok[m->expr.off + i] == lid)
            return 1;
    return 0;
}

/* Record the expression just built in the workspace as the pending value of
   local d. Falls back to storing it when it grew too long to duplicate.

   inv is the new pending expression's loop-invariance, decided by the caller:
   a plain copy of the top of the stack keeps that slot's inv, while add_loc /
   inc_loc build on the variable L<d> itself - a loop-carried leaf - so they
   always pass 0. */
static void local_set(int d, int mark, int inv)
{
    WhaleLocal *m = &whale_lmem[d];

    m->expr.off = mark;
    m->expr.len = JS_WhaleTokSinkLen() - mark;
    m->expr.inv = (uint8_t)inv;
    m->live = 1;
    m->used = 0;
    if (whale_dbg_env("WHALE_MEM_TRACE"))
        fprintf(stderr, "[set] L%d off=%d len=%d\n", d, m->expr.off, m->expr.len);
    /* A local read several times inside a loop must not stay deferred: every
       read inlines the whole pending expression, so `v = i & 255` read four
       times becomes four copies of the full conversion chain. The pass-1 heat
       (reads + 2*writes inside loop bodies) names those locals, and storing
       at the write point - a statement boundary, where materialising is safe -
       turns the later reads into variable loads. 3 means at least three reads,
       or a read plus a write: below that the deferred form is shorter.
       Short expressions are exempt: a pending `_wv[n]` copy is 4 tokens, and
       materialising it only adds a pure-copy store (`L0 = (_wv[0])`) per
       statement - the reads that follow load the same value either way. The
       bitops accumulator chain was three such stores per trip. 8 keeps the
       per-read copy at most one short statement; anything longer still
       materialises, and the length ceiling above remains the backstop.
       Self-copies are NOT exempt: `L0 = (L0)` deferred makes every later
       read inline `(L0)` instead of loading the variable, and measured on
       ident that loses 86 -> 120ms+. local_self_ref() above tells the two
       apart (a `_wv[n]` copy names no local). */
    if (m->expr.len > whale_expr_max ||
        (m->expr.len > 8 && !local_self_ref(d) && whale_loop_depth > 0 &&
         d >= 0 && d < WHALE_JIT_MAX_SLOTS && whale_loop_heat[d] >= 3))
        local_materialize(d);
}

/* Materialise every deferred local. Called at a basic-block boundary, where the
   values have to be readable from the variables again. */
static void local_flush_all(int n)
{
    int d;

    /* flush closes any open comma sequence first (a block boundary ends it),
       then materialises (which may open a new one - harmless, the next
       boundary closes it again). */
    whale_comma_close();
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
/* The body's own output. Every statement the body lowers commits on its own,
   and those commits must not interleave with the call site's open fold mark:
   an inner commit lands its tokens in the output before the outer mark's own
   tokens (the guard), and the outer fold_commit then re-copies the whole
   workspace range - body tokens a second time, plus the stray get_arg
   expressions, which are not statements and stop the parser. So the body
   writes into this buffer, appended to the real output only on success. A
   refused body is discarded by dropping the buffer, which is also what makes
   the call site's fallback sound: nothing the body emitted can survive it. */
static int whale_inline_tok[WHALE_TOK_MAX];
static double whale_inline_num[WHALE_TOK_MAX];
static int whale_inline_len;

/* Speculative var_ref inlining with a body-size gate (see whale_inline_body):
   small callees expand behind an identity guard, heavy ones keep the engine
   fast-call. Correct and fully wired; the gate keeps it profitable. */
static int whale_vr_inline_on(void)
{
    return 1;
}
/* The argument slots as the call site saw them. The body evaluates straight into
   the caller's stack above arg_base, so its own pushes and pops rewrite cur[],
   whale_vexpr/whale_vmat/whale_vname and whale_vname for exactly those slots -
   the second get_arg of the same parameter then read the first get_field's NUM
   result as the object's type and refused. Reading the snapshot instead keeps
   every argument the type it arrived with, no matter how far the body got.

   The snapshot is also what a refused body puts back: the call site truncates
   the tokens and falls back to the general call path, which re-reads those same
   slots - un-restored, it would box an object argument as a number. */
static uint8_t whale_inline_argt[4];
static WhaleVExpr whale_inline_argexpr[4];
static uint8_t whale_inline_argmat[4];
static int whale_inline_argname[4];
static int whale_inline_nested;      /* one level at a time: see the call site */
/* Declared again further down with the generator's own state. The inliner hands
   out slots from both, which is safe because a single linear pass allocates from
   them - the two areas cannot collide. */
int whale_objs;
int whale_fields;
/* The struct layout an inlined callee's object literal is built with: the
   caller sets it before whale_inline_body and clears it after. The fresh
   window lives here too, because the inlined body's define_field/put_field
   stores and get_field reads go through the same _sdp path as the main
   loop's. Reset at the top of the emit pass. */
int whale_str_atoms[WHALE_ATOM_MAX];
int whale_str_n = 0;
int whale_str_fresh = -1;
int whale_str_fresh_n = 0;
int whale_str_fresh_atoms[WHALE_ATOM_MAX];
int whale_inline_str_atoms[WHALE_ATOM_MAX];
int whale_inline_str_n = 0;
/* In-place factory fusion at the vr-inline call site: when the callee is a
   same-shape factory (`return { a: o.a, ... }`, result columns == one object
   argument's columns), the call site skips whale_inline_body entirely and
   emits, behind the same VRFN identity guard, a single field-cache store of
   the changed column into the SOURCE object. No StructNew, no call, and no
   copy stores at all: the copied columns already hold the right values, so
   rewriting them would only dirty the columns later reads need. The result
   IS the source slot, so no return copy and no read-write aliasing. A
   proxy/setter receiver fails the column resolve and deopts, which is the
   trigger. State set by the detector, read by the emitter. */
static int whale_fuse_vidx = -1;
static int whale_fuse_argc = 0;
static int whale_fuse_srcslot = -1;
static int whale_fuse_srcarg = -1;   /* which caller argument is the source */
static int whale_fuse_n = 0;
static int whale_fuse_atoms[WHALE_ATOM_MAX];
static int whale_fuse_numk = -1;     /* which caller argument is the update */
static int whale_fuse_upd = -1;      /* which literal column takes the update */

static int whale_str_col(const int *atoms, int n, int atom);
static int whale_fuse_probe(const JSWhaleFuncInfo *callee, const uint8_t *cpc,
                            int clen, uint8_t *cur, int var_count, int sp,
                            int argc, int vidx);
static int whale_inline_body(const JSWhaleFuncInfo *callee, uint8_t *cur,
                             int var_count, int arg_base)
{
    const uint8_t *pc = callee->byte_code;
    int len = callee->byte_code_len;
    int pos, sp = 0, i, ret = -1;
    int saved_argmap[4];
    int saved_objs = whale_objs, saved_fresh = whale_str_fresh;
    int saved_vserial = whale_vserial;
    int *saved_out_tok = whale_out_tok;
    double *saved_out_num = whale_out_num;
    int *saved_out_len = whale_out_len;

    if (callee->var_count != 0)
        return -1;                      /* locals would need slot remapping */
    if (callee->arg_count > 4)
        return -1;
    /* Body-size gate: a heavy body costs more inlined (every statement through
       memory under TCC) than the ~50ns call it replaces. createElement (20
       bytes, 3 fields incl. an object value and null) already loses to the
       fast call; setField (52 bytes) worse. Only tiny bodies inline. Refused
       bodies fall back to the fast call, which is already in place. */
    if (len > 12)
        return -1;
    /* Statements the body lowers go to its own buffer (see its declaration);
       the switch also covers a body inside a loop body, where the enclosing
       output is the loop's buffer. */
    whale_inline_len = 0;
    whale_out_tok = whale_inline_tok;
    whale_out_num = whale_inline_num;
    whale_out_len = &whale_inline_len;
    for (i = 0; i < 4; i++) {
        saved_argmap[i] = whale_inline_argmap[i];
        whale_inline_argmap[i] = (i < callee->arg_count) ? arg_base + i : -1;
    }
    for (i = 0; i < callee->arg_count && i < 4; i++) {
        int cslot = arg_base + i;

        whale_inline_argt[i] = cur[var_count + cslot];
        whale_inline_argexpr[i] = whale_vexpr[cslot];
        whale_inline_argmat[i] = whale_vmat[cslot];
        whale_inline_argname[i] = whale_vname[cslot];
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
                int t;

                if (a < 0 || a >= callee->arg_count || whale_inline_argmap[a] < 0)
                    goto done;
                /* The argument carries the type it has at the call site - a
                   numeric stays a number, an object stays an object slot.
                   Hard-typing it WHALE_T_NUM (the old behaviour) refused every
                   callee whose parameter is an object: setField(o, v) died on
                   its first get_field.

                   The value comes from the snapshot too, not from the caller's
                   live fold state: the body evaluates into those very slots, so
                   a re-read would pick up whatever the body last pushed there. */
                t = whale_inline_argt[a];
                if (whale_dbg_env("WHALE_JIT_DUMP"))
                    fprintf(stderr, "[vr-inline] get_arg a=%d map=%d t=%d\n",
                            a, whale_inline_argmap[a], t);
                fm = fold_mark();
                if (whale_inline_argmat[a])
                    wt_vname(whale_inline_argname[a]);
                else
                    fold_inline_expr(&whale_inline_argexpr[a]);
                /* The snapshot is a whole WhaleVExpr, inv included: a literal or
                   argument at the call site stays loop-invariant in the body. */
                fold_set(sp, fm, whale_inline_argexpr[a].inv);
                cur[var_count + sp] = (uint8_t)t;
                sp++;
            }
            break;

        case OP_object:
            {
                int os;

                if (whale_objs >= WHALE_CALL_BASE)
                    goto done;
                os = whale_objs++;
                if (whale_inline_str_n > 0) {
                    /* The caller pre-scanned this callee's define_field names:
                       build a real struct whose columns travel with it, and
                       let the initialiser stores below go through the data
                       pointer - the same shape the main loop emits for a
                       factory body. Without this an inlined factory pays
                       ObjNew + one helper per field, which eats the entire
                       win from not crossing into the engine. */
                    int k, afm = fold_mark();

                    for (k = 0; k < whale_inline_str_n; k++) {
                        wt_arr("wa", k); wt('='); wt_int(whale_inline_str_atoms[k]); wt(';');
                    }
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStructNew");
                    wt('('); wt_id("ctx"); wt(','); wt_id("_wsc"); wt('.');
                    wt_id("wa"); wt(',');
                    wt_int(whale_inline_str_n); wt(','); wt_int(os); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    wt_id("_sdp"); wt('='); wt_id("JS_WhaleStructDataPtr");
                    wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(0); wt(')'); wt(';');
                    fold_commit(afm);
                    whale_str_fresh = os;
                    whale_str_fresh_n = whale_inline_str_n;
                    for (k = 0; k < whale_inline_str_n; k++)
                        whale_str_fresh_atoms[k] = whale_inline_str_atoms[k];
                } else {
                    fm = fold_mark();
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

        case OP_define_field:
            {
                int atom = (int)whale_get_u32(pc + pos + 1);

                if (sp < 2)
                    goto done;
                ta = cur[var_count + sp - 2];
                tb = cur[var_count + sp - 1];
                if (!WHALE_IS_OBJ(ta))
                    goto done;
                /* An object value (a nested props object, a string key): hand
                   it to the engine like the main loop's ObjDefineValue arm
                   does. Only parked values travel - anything else refuses. */
                if (!whale_type_is_num(tb)) {
                    if (!WHALE_IS_VAL(tb) && !WHALE_IS_OBJ(tb))
                        goto done;
                    fm = fold_mark();
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineValue"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(','); wt_int(atom);
                    wt(','); wt_int(WHALE_IS_VAL(tb) ? WHALE_VAL_SLOT(tb) : WHALE_OBJ_SLOT(tb));
                    wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    sp--;
                    break;
                }
                /* The fresh window covers the inlined initialiser too. */
                if (whale_str_fresh == WHALE_OBJ_SLOT(ta)) {
                    int col = whale_str_col(whale_str_fresh_atoms, whale_str_fresh_n, atom);

                    if (col >= 0) {
                        fm = fold_mark();
                        wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                        wt_op(JS_OP_DOT); wt_id("tag"); wt('='); wt_int(8); wt(';');
                        wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                        wt_op(JS_OP_DOT); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt('=');
                        fold_inline(sp - 1); wt(';');
                        fold_commit(fm);
                        sp--;
                        break;
                    }
                }
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
                if (!WHALE_IS_SLOT(ta) || whale_fields >= WHALE_FIELD_CACHE) {
                    if (whale_dbg_env("WHALE_JIT_DUMP"))
                        fprintf(stderr, "[vr-inline] get_field: ta=%d fields=%d\n", ta, whale_fields);
                    goto done;
                }
                fc = whale_fields++;
                nm = whale_vserial++;
                /* not a literal we can prove stable: keep the shape check */
                fold_obj_field_load(WHALE_SLOT(ta), atom, fc, -1, nm, 0);
                whale_vname[sp - 1] = nm;
                whale_vmat[sp - 1] = 1;
                whale_vexpr[sp - 1].inv = 0;   /* a memory read */
                cur[var_count + sp - 1] = WHALE_T_NUM;
            }
            break;

        case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
            {
                fm = fold_mark();
                wt_int(op - OP_push_0);
                fold_set(sp, fm, 1);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;

        /* null / "" as a field value (vnode records hold both): park it like
           the main loop does, so the following define_field sees a VAL. */
        case OP_null: case OP_push_empty_string:
            {
                int os;

                if (whale_objs >= WHALE_CALL_BASE)
                    goto done;
                os = whale_objs++;
                fm = fold_mark();
                wt_id(op == OP_null ? "JS_WhaleObjSlotNull"
                                    : "JS_WhaleObjSlotEmptyString");
                wt('('); wt_id("ctx"); wt(',');
                wt_int(os); wt(')'); wt(';');
                fold_commit(fm);
                cur[var_count + sp] = WHALE_VAL_T(os);
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
                {
                    /* Both operands' invariance, not just the last one inlined:
                       the result is invariant only if both sides are. */
                    int inva = whale_vexpr[sp - 2].inv;
                    int invb = whale_vexpr[sp - 1].inv;

                    fold_inline(sp - 2); wt_op(opid); fold_inline(sp - 1);
                    fold_set(sp - 2, fm, inva && invb);
                }
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
                if (WHALE_IS_STRUCT(ta)) {
                    /* The layout lives in this compilation's frame columns, and the
                       caller is in the same compilation - so the result travels as
                       the type itself. No copy into an object slot and no round trip
                       through the heap: an inlined `make()` hands its object to the
                       caller as the columns it was already written to.

                       This is what lets an object that "came from another function"
                       be laid out: after inlining there is no other function, and
                       nothing in between can change a property table. */
                    ret = ta;
                } else if (WHALE_IS_SLOT(ta)) {
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
                    int fm = 0;

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
    if (whale_dbg_env("WHALE_JIT_DUMP") && ret < 0)
        fprintf(stderr, "[vr-inline] refused at pos=%d op=%d (%s) sp=%d\n",
                pos, pc[pos], whale_op_name((int)pc[pos]), sp);
    if (ret < 0) {
        /* A refused body leaves its mark on the caller's state: the slots it
           evaluated into, the object slot its OP_object may have taken, and a
           fresh struct window whose StructNew tokens the caller is about to
           truncate away. The call site falls back to the general call path,
           which reads all of these as they were at entry - so put them back. */
        for (i = 0; i < callee->arg_count && i < 4; i++) {
            int cslot = arg_base + i;

            cur[var_count + cslot] = whale_inline_argt[i];
            whale_vexpr[cslot] = whale_inline_argexpr[i];
            whale_vmat[cslot] = whale_inline_argmat[i];
            whale_vname[cslot] = whale_inline_argname[i];
        }
        whale_objs = saved_objs;
        whale_str_fresh = saved_fresh;
        whale_vserial = saved_vserial;
        whale_inline_len = 0;           /* a refused body's statements die here */
    }
    /* back to the enclosing output whatever happened: on success the buffer
       stays filled and the call site appends it - after any guard it still has
       to emit, which is why the append is not done here. The call site's
       truncate of the workspace then drops nothing but stray argument tokens. */
    whale_out_tok = saved_out_tok;
    whale_out_num = saved_out_num;
    whale_out_len = saved_out_len;
    for (i = 0; i < 4; i++)
        whale_inline_argmap[i] = saved_argmap[i];
    return ret;
}
/* Append a successful body's buffered statements to the enclosing output. The
   call site calls this after emitting whatever must precede the body in program
   order - the VRFN guard, whose emission had to wait until the body was known
   to inline, because a committed statement cannot be taken back on refusal. */
static void whale_inline_out_append(void)
{
    int i;

    for (i = 0; i < whale_inline_len && *whale_out_len < WHALE_TOK_MAX; i++) {
        whale_out_num[*whale_out_len] = whale_inline_num[i];
        whale_out_tok[(*whale_out_len)++] = whale_inline_tok[i];
    }
}
/* The observations for the function being generated: the generator compiles one
   function at a time, so this does not need to travel through every call. */
static const void *whale_prof_now;
/* Generic (unspecialized) compile mode: 1 forces every speculation summary
   down (shape_stable, prof-mono, int-region) so the function compiles to the
   fallback shape instead of bailing. Set per compilation at gen entry. */
static int whale_generic_mode;

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
        int fa[WHALE_ATOM_MAX];
        int fn = JS_WhaleShapeFields(shapes[i], fa, WHALE_ATOM_MAX);

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

/* Same-shape factory probe for in-place update at the vr-inline call site.
   Answers whether the callee `return { a: o.a, ... }` copies every field from
   one object argument (the source) except one column that takes a numeric
   argument (the update) - the setField shape. Simulates the body's stack with
   types only (no emission): get_arg takes the caller's slot type, get_field
   requires the source slot, define_field on the fresh literal records which
   argument fed each column. On success fills whale_fuse_* (vidx/argc/src
   slot/src arg/literal columns/changed-column index) and returns 1; the
   emitter then expands the call into one field-cache store on the source
   slot, with no call and no allocation. Anything else returns 0 for the
   normal vr-inline / fast-call path. */
static int whale_fuse_probe(const JSWhaleFuncInfo *callee, const uint8_t *cpc,
                            int clen, uint8_t *cur, int var_count, int sp,
                            int argc, int vidx)
{
    int cpos, csz, ndef = 0;
    int atoms[WHALE_ATOM_MAX];
    int fed[WHALE_ATOM_MAX];
    int k, csp = 0;
    uint8_t ctyp[WHALE_JIT_MAX_SLOTS];
    int8_t csrc[WHALE_JIT_MAX_SLOTS];
    int srcarg = -1, numarg = -1;

    for (k = 0; k < WHALE_JIT_MAX_SLOTS; k++)
        csrc[k] = -1;

    if (argc != callee->arg_count || argc <= 0 || argc > 4)
        return 0;   /* positional mapping below assumes an exact match */
    for (k = 0; k < WHALE_ATOM_MAX; k++)
        fed[k] = -2;
    /* pass 1: the literal columns, in definition order */
    for (cpos = 0; cpos < clen; ) {
        int cop = cpc[cpos];

        csz = whale_op_size_of(cop);
        if (csz <= 0 || cpos + csz > clen)
            return 0;
        if (cop == OP_define_field) {
            if (ndef >= WHALE_ATOM_MAX)
                return 0;
            atoms[ndef++] = (int)whale_get_u32(cpc + cpos + 1);
        } else if (cop != OP_object && cop != OP_get_arg &&
                   cop != OP_get_arg0 && cop != OP_get_arg1 &&
                   cop != OP_get_arg2 && cop != OP_get_arg3 &&
                   cop != OP_get_field && cop != OP_return) {
            return 0;   /* not a pure factory body */
        }
        cpos += csz;
    }
    if (ndef <= 0 || ndef > WHALE_ATOM_MAX)
        return 0;
    /* pass 2: simulate types. Exactly one object argument may feed fields. */
    for (cpos = 0; cpos < clen; ) {
        int cop = cpc[cpos];

        csz = whale_op_size_of(cop);
        if (csz <= 0 || cpos + csz > clen)
            return 0;
        if (cop == OP_object) {
            if (csp >= WHALE_JIT_MAX_SLOTS)
                return 0;
            ctyp[csp++] = WHALE_T_UNKNOWN + 1;  /* fresh literal marker */
        } else if (cop == OP_get_arg || (cop >= OP_get_arg0 &&
                   cop < OP_get_arg0 + 4)) {
            int a = (cop == OP_get_arg) ? (int)whale_get_u16(cpc + cpos + 1)
                                        : cop - OP_get_arg0;
            int t;

            if (a < 0 || a >= callee->arg_count || a >= 4)
                return 0;
            t = cur[var_count + sp - argc + a];
            if (csp >= WHALE_JIT_MAX_SLOTS)
                return 0;
            /* record the argument INDEX on a side stack: the type alone
               cannot tell two object arguments apart. csrc mirrors ctyp. */
            ctyp[csp] = (uint8_t)t;
            csrc[csp] = a;
            csp++;
        } else if (cop == OP_get_field) {
            int at = (int)whale_get_u32(cpc + cpos + 1);
            int di = -1, q;

            if (csp < 1)
                return 0;
            if (!WHALE_IS_SLOT(ctyp[csp - 1]))
                return 0;
            if (srcarg >= 0 && csrc[csp - 1] != srcarg)
                return 0;   /* two object arguments feed fields: not our shape */
            srcarg = csrc[csp - 1];
            for (q = 0; q < ndef; q++)
                if (atoms[q] == at) { di = q; break; }
            if (di < 0)
                return 0;
            if (fed[di] != -2)
                return 0;   /* same column defined twice */
            fed[di] = csrc[csp - 1];
            ctyp[csp - 1] = WHALE_T_NUM;
        } else if (cop == OP_define_field) {
            int at = (int)whale_get_u32(cpc + cpos + 1);
            int di = -1, q;

            if (csp < 2)
                return 0;
            for (q = 0; q < ndef; q++)
                if (atoms[q] == at) { di = q; break; }
            if (di < 0)
                return 0;
            /* the value side decides: a source-fed number means "copied
               from the source" (its get_field already recorded the column),
               the OTHER argument fed as a number means the update. An
               ICONST/small-int temporary or any other feeder is neither:
               not our shape. */
            if (ctyp[csp - 1] == WHALE_T_NUM && csrc[csp - 1] == srcarg &&
                fed[di] == srcarg) {
                /* copy column already recorded by its get_field: nothing
                   to do, the value below IS the source's column */
            } else if (whale_type_is_num(ctyp[csp - 1]) &&
                       csrc[csp - 1] >= 0 && csrc[csp - 1] != srcarg &&
                       (numarg < 0 || csrc[csp - 1] == numarg) &&
                       fed[di] == -2) {
                fed[di] = -3;   /* the one updated column */
                numarg = csrc[csp - 1];
            } else {
                return 0;
            }
            csp--;  /* object stays */
        } else if (cop == OP_return) {
            break;
        }
        cpos += csz;
    }
    /* every literal column defined exactly once; all but one copy the single
       source argument; the remaining one takes a numeric argument. */
    {
        int ncopy = 0, nupd = 0, j;

        if (srcarg < 0)
            return 0;
        /* in-place means shared storage: the arguments must be caller-owned.
           A slot the callee's own evaluation produced (an un-parked fresh
           literal, a folded temporary) would be clobbered or aliased by the
           store that follows. Only the patterns the loop actually feeds are
           accepted: the source is a parked object (a local, or a value the
           loop carried), the update is a plain number. */
        if (!WHALE_IS_SLOT(cur[var_count + sp - argc + srcarg]))
            return 0;
        if (numarg < 0 || numarg == srcarg)
            return 0;
        if (!whale_type_is_num(cur[var_count + sp - argc + numarg]))
            return 0;
        for (j = 0; j < ndef; j++) {
            if (fed[j] == srcarg)
                ncopy++;
            else if (fed[j] == -3)
                nupd++;
            else
                return 0;
        }
        if (ncopy != ndef - 1 || nupd != 1)
            return 0;
    }
    whale_fuse_vidx = vidx;
    whale_fuse_argc = argc;
    whale_fuse_srcslot = WHALE_SLOT(cur[var_count + sp - argc + srcarg]);
    whale_fuse_srcarg = srcarg;
    whale_fuse_n = ndef;
    for (k = 0; k < ndef; k++)
        whale_fuse_atoms[k] = atoms[k];
    whale_fuse_numk = numarg;
    /* the one literal column the numeric argument fed */
    for (k = 0; k < ndef; k++)
        if (fed[k] == -3) { whale_fuse_upd = k; break; }
    return 1;
}

/* In-place emitter for a probed same-shape factory call: `st = setField(st, v)`
   becomes one field-cache store of column `whale_fuse_upd` into the SOURCE
   slot, behind the same VRFN identity guard the normal vr-inline path emits.
   No StructNew, no call, no copy stores: columns the factory copied already
   hold the source's values, so rewriting them is pure waste - and worse, it
   would dirty the columns the loop's later reads resolve through the same
   field cache. The result slot IS the source slot: the fused call returns the
   object it was given, mutated, which is exactly what the interpreter's fresh
   object contains column-for-column.

   Proxy/setter detection is the resolve itself: find_own_property answers
   NULL for an accessor and ObjFieldPtr deopts, so a trigger runs only where
   the language says a store runs - on the source object, once.

   Emits the guard first (it must precede the store), then the resolve+store
   through one fresh cache pair, then fixes up the call-site stack: the stray
   argument tokens die at `fm`, the result is the source type, sp collapses
   to func_slot+1 - the same contract the vr-inline success path honours.
   `fields_p` is the generator's own cache counter (the file-scope
   whale_fields is shadowed inside whale_jit_gen, so the emitter cannot name
   it); `expect_slot` is the guard-table index for the same reason. */
static void whale_fuse_emit(int *sp, uint8_t *cur, int var_count,
                            int func_slot, int fm, int vidx, int expect_slot,
                            int *fields_p)
{
    int fc, srcslot = whale_fuse_srcslot;
    int updat = whale_fuse_atoms[whale_fuse_upd];
    int stk = *sp - whale_fuse_argc + whale_fuse_numk;
    int gfm;

    fc = (*fields_p)++;
    /* guard: the slot must still hold the probed function */
    gfm = fold_mark();
    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleVarRefIsFn");
    wt('('); wt_id("ctx"); wt(','); wt_id("var_refs"); wt(',');
    wt_int(vidx); wt(','); wt_int(expect_slot); wt(')'); wt(')');
    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    fold_commit(gfm);
    /* resolve next: it emits its own deopts (accessor, missing column,
       non-numeric old value), and a committed store cannot be taken back -
       so the guard ordering the vr-inline path relies on applies here too,
       resolve before store, both preceded by the guard. Note the resolve
       commits itself (fold_obj_field_resolve marks and commits internally),
       so no outer mark/commit here - wrapping one around it emitted the
       resolve twice per iteration. */
      fold_obj_field_resolve(srcslot, updat, fc, -1, 0);
    /* the update value must survive as a C variable: it is read inside the
       store statement, past the argument tokens' truncation point */
    fold_materialize(stk);
    {
        int sfm = fold_mark();

        wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt('='); wt_int(8); wt(';');
        wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt('=');
        fold_inline(stk); wt(';');
        fold_commit(sfm);
    }
    JS_WhaleTokSinkTruncate(fm); whale_comma_open = 0;
    cur[var_count + func_slot] = cur[var_count + *sp - whale_fuse_argc + whale_fuse_srcarg];
    *sp = func_slot + 1;
}

/* The layout a settled object has: the attributes its observed shapes carry
   often enough, most-covered first. Coverage is per distinct shape (the same
   shape seen at ten sites counts once - it is one layout, not ten votes), and
   the bar is 40% of shapes - lower than the old unanimous-mono rule, so a
   mildly polymorphic program still lays out instead of falling back to the map
   for everything. The gcc second compilation drops the bar to zero (every
   known attribute, memory permitting): its code is better enough to pay for
   wider structs.

   The shapes are the *access* sites' shapes, not a creation point - what was
   observed is what the fields are, so it does not matter which function
   created the object. That is what lets an object that arrives as a call's
   result be laid out like one built here. */
static int whale_layout_from_sites(const int *sites, int nsites, int *atoms, int max,
                                     int pct)
{
    const void *shapes[WHALE_ATOM_MAX];
    int nshapes = 0, s, i;
    int cand[WHALE_ATOM_MAX], cnt[WHALE_ATOM_MAX], n = 0;

    /* pct <= 0 means every known attribute (the gcc second compilation). */
    for (s = 0; s < nsites; s++) {
        const void *one[8];
        int on, k;

        if (sites[s] == 0 || whale_prof_now == NULL)
            continue;
        on = JS_WhaleProfShapes(whale_prof_now, (unsigned)sites[s], one, 8);
        for (k = 0; k < on && nshapes < WHALE_ATOM_MAX; k++) {
            int m, seen = 0;

            for (m = 0; m < nshapes; m++)
                if (shapes[m] == one[k]) { seen = 1; break; }
            if (!seen)
                shapes[nshapes++] = one[k];
        }
    }
    for (s = 0; s < nshapes; s++) {
        int fa[WHALE_ATOM_MAX];
        int fn = JS_WhaleShapeFields(shapes[s], fa, WHALE_ATOM_MAX);
        int q;

        for (q = 0; q < fn && n < WHALE_ATOM_MAX; q++) {
            int m, seen = 0;

            for (m = 0; m < n; m++)
                if (cand[m] == fa[q]) { seen = 1; break; }
            if (seen)
                continue;
            {
                int c = 0, t;

                for (t = 0; t < nshapes; t++) {
                    int fb[WHALE_ATOM_MAX];
                    int fm = JS_WhaleShapeFields(shapes[t], fb,
                                                 WHALE_ATOM_MAX);
                    int u;

                    for (u = 0; u < fm; u++)
                        if (fb[u] == fa[q]) { c++; break; }
                }
                if (pct > 0 && nshapes > 0 && c * 100 < pct * nshapes)
                    continue;
            }
            cand[n] = fa[q];
            cnt[n] = 0;
            {
                int t;

                for (t = 0; t < nshapes; t++) {
                    int fb[WHALE_ATOM_MAX];
                    int fm = JS_WhaleShapeFields(shapes[t], fb,
                                                 WHALE_ATOM_MAX);
                    int u;

                    for (u = 0; u < fm; u++)
                        if (fb[u] == fa[q]) { cnt[n]++; break; }
                }
            }
            n++;
        }
    }
    /* most-covered first; truncation then keeps the hottest columns */
    for (i = 0; i < n; i++) {
        int j, bj = i;

        for (j = i + 1; j < n; j++)
            if (cnt[j] > cnt[bj])
                bj = j;
        if (bj != i) {
            int t = cand[i]; cand[i] = cand[bj]; cand[bj] = t;
            t = cnt[i]; cnt[i] = cnt[bj]; cnt[bj] = t;
        }
    }
    {
        int k = n < max ? n : max;

        for (i = 0; i < k; i++)
            atoms[i] = cand[i];
        return k;
    }
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

        if (whale_dbg_env("WHALE_PROF_DBG") && dbg < 40) {
            int fa[WHALE_ATOM_MAX];
            int fn = whale_struct_fields(off, fa, WHALE_ATOM_MAX);
            int q;

            dbg++;
            fprintf(stderr, "[prof] lookup off=%u prof=%p -> n=%d%s fields=%d [",
                    off, whale_prof_now, n, (n == 1 ? " STABLE" : ""), fn);
            for (q = 0; q < fn; q++)
                fprintf(stderr, "%s%d", q ? "," : "", fa[q]);
            fprintf(stderr, "]\n");
        }
        /* generic mode never claims a single shape (forces the full-resolve
           fallback instead of the two-state fast path). */
        if (whale_generic_mode)
            return 0;
        return n == 1;
    }
}

/* Did the warm-up see exactly two shapes at this site (small polymorphic IC)?
   Two - not three or four (those go mega: the single-pair full resolve) and
   not zero/one (single-pair paths already cover those). Negative (given up)
   is not two. */
static int whale_prof_two_shapes(unsigned off)
{
    const void *sh[8];
    int n;

    if (off == 0 || whale_prof_now == NULL)
        return 0;
    n = JS_WhaleProfShapes(whale_prof_now, off, sh, 8);
    return n == 2;
}
static unsigned whale_prof_two_off;

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
    if (whale_dbg_env("WHALE_PROF_DBG"))
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
    int dbg = whale_dbg_env("WHALE_STEP_DBG") != NULL;

    /* Bisect switch: WHALE_STEP_OFF=<opcode> refuses that one opcode, so a single
       hand-over can be turned off without touching the rest. It is how "object in
       a value position" was isolated, and it stays for the next one.
       (Diagnostic: debug builds only.) */
#ifdef WHALE_DEBUG
    {
        const char *off = getenv("WHALE_STEP_OFF");

        if (off && atoi(off) == op)
            return 0;
    }
#endif

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
           exactly what that wants to be. Operand 1+ used to be refused when it was
           not a number, because `out[i] = t` was said to generate code TinyCC would
           not parse where `out[i] = 3` was fine.

           That is not what is happening. With the rule off, TinyCC accepts the
           generated code and the failure is a run-time access violation once the
           first call has returned - no compile error at all. See
           notes/whale-step-repro.md. The rule stays default-on while that is being
           found; WHALE_VALUE_OBJ=1 turns it off, which is how the reproducer is
           reached without editing this file. */
        if (i > 0 && !whale_type_is_num(t) && !whale_dbg_env("WHALE_VALUE_OBJ")) {
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

/* Was every field-access site in this function observed holding a single shape?
   If so, the objects those sites touch have a settled layout. A function with no
   sites at all answers no: nothing was observed, so nothing is settled. */
static int whale_sites_mono(const int *sites, int nsites)
{
    int i;

    if (nsites == 0)
        return 0;
    for (i = 0; i < nsites; i++)
        if (!whale_prof_site_ok((unsigned)sites[i]))
            return 0;
    return 1;
}

/* Frequency gate: like mono, but a polymorphic site (up to 4 shapes, the
   recorder's cap) is accepted instead of refused. The layout then keeps only
   the attributes seen often enough (see whale_layout_from_sites) and the rest
   fall back to the dynamic map - which already exists for exactly this. An
   explicitly disqualified site (negative) still refuses, as does an empty
   site list. */
static int whale_sites_freq(const int *sites, int nsites)
{
    const void *sh[8];
    int i, n;

    if (nsites == 0)
        return 0;
    for (i = 0; i < nsites; i++) {
        if (sites[i] == 0 || whale_prof_now == NULL)
            return 0;
        n = JS_WhaleProfShapes(whale_prof_now, (unsigned)sites[i], sh, 8);
        if (n < 0 || n > WHALE_PROFILE_SHAPES)
            return 0;
    }
    return 1;
}

static int whale_jit_gen(JSContext *ctx, const JSWhaleFuncInfo *info)
{
    whale_prof_now = info->prof;
    /* Generic (unspecialized) mode: force every speculation summary down so
       the function compiles to the fallback shape (full-resolve IC, generic
       objects, double loop) instead of bailing to the interpreter. Driven by
       the version table (info->generic) or forced via WHALE_GENERIC. */
    whale_generic_mode = (info->generic != 0) ||
        (whale_dbg_env("WHALE_GENERIC") != 0);
    /* A linear disassembly, as opcode numbers and offsets. The generator's own view
       of the bytecode is the only one that matters when a case is refused, and
       reading it back out of the switch is guesswork. */
    if (whale_dbg_env("WHALE_OP_DBG")) {
        int k = 0;

        fprintf(stderr, "[consts] if_false8=%d if_true8=%d goto8=%d if_false=%d goto=%d catch=%d throw=%d put_field=%d get_field=%d get_field2=%d mod=%d\n",
                OP_if_false8, OP_if_true8, OP_goto8, OP_if_false, OP_goto, OP_catch, OP_throw,
                OP_put_field, OP_get_field, OP_get_field2, OP_mod);
        fprintf(stderr, "[ops] len=%d:", info->byte_code_len);
        while (k < info->byte_code_len && k < 60) {
            int o = info->byte_code[k];
            int s = whale_op_size_of(o);

            fprintf(stderr, " %d:%s", k, whale_op_name(o));
            if (s <= 0)
                break;
            k += s;
        }
        fprintf(stderr, "\n");
    }
    /* The loops the compiler gave us. Printed here so the generator side of the
       hand-over can be checked without emitting anything yet. */
    if (whale_dbg_env("WHALE_LOOP_DBG") && info->loop_count > 0) {
        int li;

        fprintf(stderr, "[gen-loops] blen=%d count=%d:", info->byte_code_len,
                info->loop_count);
        for (li = 0; li < info->loop_count; li++)
            fprintf(stderr, " {head=%d back=%d exit=%d cond=%d}",
                    info->loops[li].head, info->loops[li].back,
                    info->loops[li].exit, info->loops[li].kind);
        fprintf(stderr, "\n");
    }
    /* What JS_UNDEFINED actually looks like, measured rather than assumed. The tag
       value, the word boundaries and the width are all things to read off the engine,
       and guessing them has already cost several rounds. */
    if (whale_dbg_env("WHALE_UNDEF_DBG")) {
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
    if (whale_dbg_env("WHALE_JIT_DUMP"))
        fprintf(stderr, "[gen] argc=%d varc=%d len=%d prof=%p\n",
                info->arg_count, info->var_count, info->byte_code_len, info->prof);
    whale_gen_self_ref = 0;
    {
        static int once;
        if (!once && whale_dbg_env("WHALE_JIT_DUMP")) {
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
    uint8_t *args_arr = NULL;   /* per argument: read as an array (length/index) */
    int *arg_obj_slot = NULL;   /* per argument: the slot it travels in, or -1 */
    int *loc_slot = NULL;       /* per local: its own slot when it is reassigned */
    int *loop_back = NULL;      /* per position: the backward jump that closes it */
    uint8_t var_ref_used[4] = { 0, 0, 0, 0 };
    uint8_t var_ref_val[4] = { 0, 0, 0, 0 };   /* closure reading: the slot is read as a value */
    uint8_t var_ref_rw[4] = { 0, 0, 0, 0 };    /* any value read or write: the slot gets a pval */
    unsigned *assign_cnt = NULL;   /* weighted access count */
    int unreachable = 0;
    int ret_is_bool = -1;
    int whale_objs = 0;   /* object slots handed out; each new object takes one */
    int whale_has_call = 0; /* a call clobbers registers, so no pinning then */
    int whale_has_array = 0; /* typed-array element access: keep its cache group */
    int whale_fields = 0;   /* object-property cache pairs handed out */    int whale_shape_stable = 0; /* object shapes cannot change after this point */
    int whale_ret_obj = 0;      /* the function can return an array or object */
    int whale_def_atoms[WHALE_ATOM_MAX];
    int whale_put_atoms[WHALE_ATOM_MAX];
    int whale_def_atoms_n = 0;
    int whale_put_atoms_n = 0;
    /* Loop-level stability (notes <114>): per-head flag, 1 when the loop body
       itself adds no property (every in-body put hits the define set plus
       pre-loop puts) and holds no call. Lets a loop go stable even when a
       pre-loop store (e.g. `o.r = 0` before the loop) kills the function
       level. Indexed by head like loop_back[]; allocated/freed with it. */
    uint8_t *whale_loop_stable = NULL;
    int whale_loop_predef[WHALE_ATOM_MAX];
    int whale_loop_predef_n = 0;
    int whale_call_pos[WHALE_CALL_MAX];
    int whale_call_n = 0;
    /* The innermost enclosing `catch` targets, innermost last. The bytecode already
       says where each one goes, so this is compile-time information only - what the
       generated code needs is "which label do I jump to". Empty means no handler is
       open, and then there is nothing to catch: the operation deopts as before. */
    int whale_catch_tgt[WHALE_CATCH_MAX];
    int whale_catch_depth = 0;
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
       by every field access on that slot.
       These four are file-level statics (like whale_objs/whale_fields below):
       whale_inline_body lays out and reads an inlined callee's object too, and
       it is a separate function. Reset at the top of the emit pass. */
    int whale_str_slot = -1;
    int whale_str_L[WHALE_ATOM_MAX];
    /* Per closure slot: the object slot its value is parked in, or -1. Filled
       in the prologue for the slots this reading reads as values. */
    int whale_vr_slot[4];
    /* Speculative inline of a closure-slot call target: the function the slot
       held at compile time, its info, and the guard-table index. */
    void *whale_vr_fn[4] = { 0 };
    int whale_vr_expect_slot[4] = { -1, -1, -1, -1 };
    JSWhaleFuncInfo whale_vr_info[4];
    unsigned whale_plain_slots = 0;
    int prev_op;    /* the opcode before the current one, in the scan pass */
    /* The one object slot this layout is materialised into when it has to become a
       real JSValue - as a call argument, say. One slot, not one per materialisation
       site: two sites reaching the same jump target would otherwise carry different
       slot numbers, meet as different types and merge to UNKNOWN, which throws the
       whole function away. That is exactly what happened to immutable_update. */
    int whale_str_obj_slot = -1;

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
    args_arr = calloc(1, arg_count > 0 ? arg_count : 1);
    arg_obj_slot = malloc(sizeof(int) * (arg_count > 0 ? arg_count : 1));
    loc_slot = malloc(sizeof(int) * (var_count > 0 ? var_count : 1));
    assign_cnt = calloc(var_count > 0 ? var_count : 1, sizeof(unsigned));
    whale_vexpr = calloc(stack_size, sizeof(WhaleVExpr));
    whale_vmat = calloc(1, stack_size);
    whale_lmem = calloc(var_count > 0 ? var_count : 1, sizeof(WhaleLocal));
    loop_back = malloc((len + 1) * sizeof(*loop_back));
    whale_loop_trip = calloc(len + 1, sizeof(*whale_loop_trip));
    whale_loop_trip_g = malloc((len + 1) * sizeof(*whale_loop_trip_g));
    whale_loop_trip_exit = malloc((len + 1) * sizeof(*whale_loop_trip_exit));
    whale_loop_memset = calloc(len + 1, sizeof(*whale_loop_memset));
    whale_loop_countrec = calloc(len + 1, sizeof(*whale_loop_countrec));
    whale_loop_intreg = calloc(len + 1, sizeof(*whale_loop_intreg));
    whale_loop_reuse = calloc(len + 1, sizeof(*whale_loop_reuse));
    if (!targets || !seen || !snap || !cur || !args_used || !assign_cnt ||
        !args_obj || !args_arr ||
        !whale_vexpr || !whale_vmat ||
        !whale_lmem || !loop_back || !whale_loop_trip ||
        !whale_loop_trip_g || !whale_loop_trip_exit || !whale_loop_memset ||
        !whale_loop_countrec || !whale_loop_intreg || !whale_loop_reuse)
        { whale_out_line = __LINE__; goto out; }
    for (i = 0; i <= len; i++)
        loop_back[i] = -1;
    memset(whale_lo_c, 0, sizeof(whale_lo_c));
    memset(whale_lo_cv, 0, sizeof(whale_lo_cv));
    memset(whale_loc_obj, 0, sizeof(whale_loc_obj));
    whale_obj_pending = 0;
    whale_ret_obj_g = 0;
    /* the file-level struct-layout state: one compilation owns it at a time.
       whale_vr_fn/vr_expect_slot are the previous compilation's speculative
       resolutions - a callee resolved for one function is meaningless for the
       next, and reusing its guard index would compile the wrong identity
       check into the new body. */
    whale_str_n = 0;
    whale_str_fresh = -1;
    whale_str_fresh_n = 0;
    whale_inline_str_n = 0;
    whale_sp_reset_all();
    for (i = 0; i < 4; i++) {
        whale_vr_fn[i] = NULL;
        whale_vr_expect_slot[i] = -1;
    }

    /* pass 1: jump targets + which arguments are read */
    /* The previous opcode, for the one shallow test this pass makes about a return
       (see OP_return below). */
    prev_op = -1;
    for (pos = 0; pos < len; ) {
        int op = pc[pos];
        int sz = whale_op_size_of(op);

        if (sz <= 0 || pos + sz > len)
            { whale_out_line = __LINE__; goto out; }
        if (op == OP_put_array_el || op == OP_get_array_el)
            whale_has_array = 1;
        if (op == OP_goto8 || op == OP_if_false8 || op == OP_if_true8) {
            int tgt = pos + 1 + (int8_t)pc[pos + 1];

            if (whale_dbg_env("WHALE_OP_DBG"))
                fprintf(stderr, "[tgt8] pos=%d op=%d tgt=%d\n", pos, op, tgt);
            targets[tgt] = 1;
            if (tgt < pos)              /* backward jump: a loop back edge */
                loop_back[tgt] = pos;
        } else if (op == OP_goto || op == OP_if_false || op == OP_if_true) {
            int tgt = pos + 1 + (int32_t)whale_get_u32(pc + pos + 1);

            if (whale_dbg_env("WHALE_OP_DBG"))
                fprintf(stderr, "[tgt] pos=%d op=%d tgt=%d\n", pos, op, tgt);
            targets[tgt] = 1;
            if (tgt < pos)
                loop_back[tgt] = pos;
        } else if (op == OP_catch) {
            /* `catch L` is a jump target too: it is where the handler starts. Without
               this the label is never emitted and a `goto L` from a `throw` - or any
               later op that jumps into the catch block - has nowhere to land. */
            int ctgt = pos + 1 + (int32_t)whale_get_u32(pc + pos + 1);

            if (whale_dbg_env("WHALE_OP_DBG"))
                fprintf(stderr, "[catch-tgt] pos=%d raw=%u tgt=%d\n",
                        pos, (unsigned)whale_get_u32(pc + pos + 1), ctgt);
            targets[ctgt] = 1;
        } else if (op >= OP_get_arg0 && op < OP_get_arg0 + 4) {
            args_used[op - OP_get_arg0] = 1;
            if (whale_next_is_field_read(pc, len, pos, sz))
                args_obj[op - OP_get_arg0] = 1;
            if (whale_feeds_array_use(pc, len, pos, sz))
                args_arr[op - OP_get_arg0] = 1;
        } else if (op == OP_get_arg) {
            int a = (int)whale_get_u16(pc + pos + 1);
            if (a >= 0 && a < arg_count) {
                args_used[a] = 1;
                if (whale_next_is_field_read(pc, len, pos, sz))
                    args_obj[a] = 1;
                if (whale_feeds_array_use(pc, len, pos, sz))
                    args_arr[a] = 1;
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
                   Weighted higher than one put_loc: a loop counter is written
                   once per iteration. */
                int a = (int)pc[pos + 1];
                if (a < var_count) {
                    assign_cnt[a] += 4;
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
        } else if ((op >= OP_set_loc0 && op <= OP_set_loc3) ||
                   op == OP_set_loc8 || op == OP_set_loc) {
            /* set_loc stores too (and leaves the value): missing it here
               miscounts a loop-carried variable as single-assignment, and the
               constant propagator then rewrites the loop body with the initial
               value - a silent wrong answer, the same class as the inc_loc
               hole fixed in <13>. */
            int a = (op == OP_set_loc8) ? (int)pc[pos + 1]
                : (op == OP_set_loc) ? (int)whale_get_u16(pc + pos + 1)
                : op - OP_set_loc0;

            if (a < var_count)
                assign_cnt[a]++;
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
        } else if (op == OP_return) {
            /* Does this function hand an object back? If so, the objects it builds have
               to be real ones - they leave, and another compilation will meet them. A
               function that returns a number keeps the frame-column path, which needs
               no allocation and is already fast.

               The test is deliberately shallow: the opcode just before the return, and
               whether that local is known to hold an object. That is the shape a factory
               has, and it is all this pass can see. */
            if ((prev_op >= OP_get_loc0 && prev_op <= OP_get_loc3 &&
                 whale_loc_obj[prev_op - OP_get_loc0]) ||
                prev_op == OP_define_field || prev_op == OP_object)
                whale_ret_obj_g = 1;
        } else if (op == OP_put_array_el) {
            /* A local object stored into an array leaves the function's own
               lifetime, so a frame-column layout gets materialised into a
               real object on every store (ObjNew + one ObjDefineNum per
               column). Marking the function as one that hands objects over
               would make the literal build a real struct directly - and that
               was tried: StructNew per iteration (one JSObject plus a block
               allocation, plus the GC scan each allocation triggers) measured
               612ms against 430 for the materialising path. The wall is
               JS_NewObjectClass itself, not the column layout, so the mark
               stays off until the allocator has a lighter path for this
               class. */        } else if (op == OP_call || (op >= OP_call0 && op <= OP_call3)) {
            whale_has_call = 1;
            /* Where this call is, for the "nothing after this point may call"
               gate the layout arms test. That gate reads whale_call_pos[last] and
               was written against an array nothing ever filled - whale_call_n
               stayed 0, so `whale_call_n == 0` short-circuited every time and the
               protection never ran. A local that is laid out and then reassigned
               from a call is what it exists to prevent: `st` in immutable_update
               was laid out at pos 0 with setField() called at pos 44, and the
               columns went stale on the first iteration. */
            if (whale_call_n < WHALE_CALL_MAX)
                whale_call_pos[whale_call_n++] = pos;
        } else if (op >= OP_get_var_ref0 && op < OP_get_var_ref0 + 4) {
            /* Only a slot used as a call target has to be this very function;
               in the other reading the same slot is read as a value, which the
               prologue parks once. */
            if (whale_var_ref_feeds_call(pc, len, pos, sz)) {
                if (whale_assume_self) {
                    var_ref_used[op - OP_get_var_ref0] = 1;
                    whale_gen_self_ref = 1;
                } else {
                    var_ref_val[op - OP_get_var_ref0] = 1;
                }
            }
            /* Every value read or write gets the slot a pval: get_var_ref and
               put_var_ref become tag-checked loads and stores through the
               cell pointer, zero helper calls per access. */
            var_ref_rw[op - OP_get_var_ref0] = 1;
        } else if (op == OP_get_var_ref) {
            int a = (int)whale_get_u16(pc + pos + 1);

            if (a >= 0 && a < 4 && whale_var_ref_feeds_call(pc, len, pos, sz)) {
                if (whale_assume_self) {
                    var_ref_used[a] = 1;
                    whale_gen_self_ref = 1;
                } else {
                    var_ref_val[a] = 1;
                }
            }
            if (a >= 0 && a < 4)
                var_ref_rw[a] = 1;
        } else if (op == OP_put_var_ref || (op >= OP_put_var_ref0 && op <= OP_put_var_ref3)) {
            int w = (op == OP_put_var_ref) ? (int)whale_get_u16(pc + pos + 1) : op - OP_put_var_ref0;

            if (w >= 0 && w < 4)
                var_ref_rw[w] = 1;
        }
        prev_op = op;
        pos += sz;
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
    whale_shape_stable = whale_generic_mode ? 0 : 1;
    /* ELS: a pinned local whose live range crosses a call pays spill+reload
       per call and risks stale reloads (its slot is only written by the
       call writeback). If any loop body contains a call - a bytecode call or
       any opcode the generator may lower to a helper call - local pinning is
       off for the whole function: the net is zero or negative. _wv pins are
       unaffected (fold temporaries never span a call). */
    whale_loop_has_call = 0;
    whale_loop_shape_dirty = 0;
    whale_loop_has_bcall = 0;
    whale_reuse_n = 0;
    memset(whale_reuse_map, 0xff, sizeof(whale_reuse_map));
    {
        int k;

        for (k = 0; k < whale_call_n; k++) {
            int tgt;

            for (tgt = 0; tgt < len; tgt++) {
                if (loop_back[tgt] >= 0 && whale_call_pos[k] >= tgt &&
                    whale_call_pos[k] <= loop_back[tgt]) {
                    whale_shape_stable = 0;
                    whale_loop_has_call = 1;
                    whale_loop_has_bcall = 1;
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
    /* Loop-level stability table: for each head, 1 when the body is stable
       even if the function is not. The body must hold no call and add no
       property: every put inside hits the define set plus puts that ran
       before the head (a pre-loop store grows the shape once, early - the
       loop itself then sees a fixed shape). Only single-loop functions take
       it (multi-loop sharing of the predef set is not worth the bookkeeping;
       they keep the function level). */
    whale_loop_stable = calloc(len + 1, 1);
    if (whale_loop_stable && info->loop_count == 1) {
        int h;

        for (h = 0; h <= len; h++) {
            int back, q, qsz, ok;

            if (loop_back[h] < 0)
                continue;
            back = loop_back[h];
            /* pre-loop puts: put_field atoms before the head. */
            whale_loop_predef_n = 0;
            for (q = 0; q < h; ) {
                int qo = (int)pc[q];

                qsz = whale_op_size_of(qo);
                if (qsz <= 0 || q + qsz > h)
                    break;
                if (qo == OP_put_field && qsz >= 5) {
                    int atom = (int)whale_get_u32(pc + q + 1);

                    if (whale_loop_predef_n < WHALE_ATOM_MAX)
                        whale_loop_predef[whale_loop_predef_n++] = atom;
                }
                q += qsz;
            }
            ok = 1;
            for (q = h; q <= back && ok; ) {
                int qo = (int)pc[q];

                qsz = whale_op_size_of(qo);
                if (qsz <= 0 || q + qsz > len)
                    break;
                if (qo == OP_put_field && qsz >= 5) {
                    int atom = (int)whale_get_u32(pc + q + 1);
                    int j, found = 0;

                    for (j = 0; j < whale_def_atoms_n && !found; j++)
                        if (whale_def_atoms[j] == atom)
                            found = 1;
                    for (j = 0; j < whale_loop_predef_n && !found; j++)
                        if (whale_loop_predef[j] == atom)
                            found = 1;
                    if (!found)
                        ok = 0;
                }
                q += qsz;
            }
            if (ok) {
                int k, incall = 0;

                for (k = 0; k < whale_call_n; k++)
                    if (whale_call_pos[k] >= h && whale_call_pos[k] <= back)
                        incall = 1;
                if (!incall)
                    whale_loop_stable[h] = 1;
            }
        }
    }
    whale_loop_stable_tab = whale_loop_stable;

    /* pass 2: emit tokens. Nothing here builds text - the generator pushes
       tokens into TinyCC's parser directly, so there is no lexer and no
       keyword table, and keywords are compile-time constants. */

    /* Which locals each loop body writes, for loop-invariance below. Built
       from the pass-1 back-edge table: one scan of [head, back] per loop.
       Freed with the rest at out:. The same scan counts reads+writes per
       local inside loop bodies (whale_loop_heat): the ELS decision below pins
       the hottest numeric locals. */
    whale_loop_written = NULL;
    whale_loop_wn = 0;
    if (var_count > 0) {
        int h;

        whale_loop_written = calloc((size_t)(len + 1) * var_count, 1);
        if (!whale_loop_written)
            { whale_out_line = __LINE__; goto out; }
        whale_loop_wn = var_count;
        memset(whale_loop_heat, 0, sizeof(whale_loop_heat));
        whale_emit_loop_count = info->loop_count;
        for (h = 0; h <= len; h++) {
            if (loop_back[h] >= 0) {
                int q = h, qsz;

                while (q <= loop_back[h]) {
                    int qo = (int)pc[q];
                    int w, r = -1;

                    qsz = whale_op_size_of(qo);
                    if (qsz <= 0 || q + qsz > len)
                        break;
                    /* any opcode that may lower to a helper call disqualifies
                       local pinning for the whole function (see
                       whale_loop_has_call above). TRIED: exempting field
                       access under a proven-stable shape (helpers run once,
                       never touch pin homes) - writeOrder gained 3ms but
                       objshape lost 17ms (pin pool pressure against long
                       conversion chains). Reverted: pinning stays numeric-only.
                       Object loops keep their field caches (two-state/poly),
                       which is where their wins come from. */
                    if (!whale_is_pure_num_op(qo))
                        whale_loop_has_call = 1;
                    w = whale_writes_local(qo, pc, q);
                    if (w >= 0 && w < var_count) {
                        whale_loop_written[(size_t)h * var_count + w] = 1;
                        if (w < WHALE_JIT_MAX_SLOTS)
                            whale_loop_heat[w] += 2;
                        /* A store into an object-holding local inside a loop
                           may land a second identity in the same slot (e.g.
                           `a = other`), reshaping it: VAL-slot field caches
                           are not stable then. put_field (field-value stores)
                           does not come through here as a local write of the
                           object itself - writes_local names the value local
                           for put_loc/set_loc/add/inc/dec, and -1 for field
                           stores - so plain value traffic is unaffected. */
                        if (w < WHALE_JIT_MAX_SLOTS && whale_loc_obj[w])
                            whale_loop_shape_dirty = 1;
                    }
                    /* A fresh object created inside a loop may be stored over
                       a cached slot on some trip: same verdict. */
                    if (qo == OP_object)
                        whale_loop_shape_dirty = 1;
                    /* reads: get_loc family (get_loc0_loc1 reads two) */
                    if (qo >= OP_get_loc0 && qo <= OP_get_loc3)
                        r = qo - OP_get_loc0;
                    else if (qo == OP_get_loc8)
                        r = (int)pc[q + 1];
                    else if (qo == OP_get_loc)
                        r = (int)whale_get_u16(pc + q + 1);
                    else if (qo == OP_get_loc0_loc1) {
                        if (0 < var_count && 0 < WHALE_JIT_MAX_SLOTS)
                            whale_loop_heat[0]++;
                        r = 1;
                    }
                    if (r >= 0 && r < var_count && r < WHALE_JIT_MAX_SLOTS)
                        whale_loop_heat[r]++;
                    q += qsz;
                }
            }
        }
    }

    /* Small-trip-count loop unrolling table, indexed by loop head position.
       whale_loop_trip[h] > 0 means the loop at h runs exactly that many times
       and loop_end() expands its body inline instead of emitting the loop.
       Only the narrow shape is recognised: `g = 0` right before the head,
       `g < K` (K a small integer literal, K <= 8) as the head test, `g++` at
       the back edge, no other write to g inside, no call inside, and no jump
       that leaves the body except through the head test's exit. Anything else
       leaves 0 and the loop is emitted as before. */
    {
        int h;

        /* NB: these are heap arrays (len+1 ints), not statics - sizeof the
           pointer would only clear 8 bytes. */
        memset(whale_loop_trip, 0, ((size_t)len + 1) * sizeof(*whale_loop_trip));
        memset(whale_loop_trip_g, 0xff, ((size_t)len + 1) * sizeof(*whale_loop_trip_g));
        memset(whale_loop_trip_exit, 0xff, ((size_t)len + 1) * sizeof(*whale_loop_trip_exit));
        memset(whale_loop_memset, 0, ((size_t)len + 1) * sizeof(*whale_loop_memset));
        memset(whale_loop_countrec, 0, ((size_t)len + 1) * sizeof(*whale_loop_countrec));
        for (h = 0; h <= len; h++) {
            if (loop_back[h] >= 0) {
                int trip = 0, g = -1, exitp = -1;

                trip = whale_loop_trip_of(pc, len, h, loop_back[h], var_count,
                                          loop_back, targets,
                                          whale_call_pos, whale_call_n,
                                          &g, &exitp);
                if (trip > 0 && h <= len) {
                    whale_loop_trip[h] = trip;
                    whale_loop_trip_g[h] = g;
                    whale_loop_trip_exit[h] = exitp;
                }
                {
                    WhaleMemsetLoop ms;

                    memset(&ms, 0, sizeof(ms));
                    if (whale_loop_memset_of(pc, len, h, loop_back[h],
                                             var_count, loop_back,
                                             whale_call_pos, whale_call_n,
                                             &ms) &&
                        h <= len)
                        whale_loop_memset[h] = ms;
                }
                {
                    WhaleCountLoop cs;

                    memset(&cs, 0, sizeof(cs));
                    cs.g = -1;
                    cs.acc = -1;
                    cs.exitp = -1;
                    if (whale_loop_count_of(pc, len, h, loop_back[h],
                                            var_count, loop_back,
                                            whale_call_pos, whale_call_n,
                                            &cs) &&
                        h <= len)
                        whale_loop_countrec[h] = cs;
                }
                {
                    WhaleIntLoop ir;

                    memset(&ir, 0, sizeof(ir));
                    ir.g = -1;
                    ir.exitp = -1;
                    if (whale_loop_int_of(pc, len, h, loop_back[h],
                                            var_count, loop_back,
                                            whale_call_pos, whale_call_n,
                                            &ir) &&
                        h <= len)
                        whale_loop_intreg[h] = ir;
                }
                {
                    WhaleReuseLoop ru;
                    int objpos = -1, arrpos = -1;

                    memset(&ru, 0, sizeof(ru));
                    if (whale_loop_reuse_of(pc, len, h, loop_back[h],
                                            var_count, loop_back,
                                            whale_call_pos, whale_call_n,
                                            &objpos, &arrpos) &&
                        h <= len &&
                        whale_loop_reuse_writeonly(pc, len, h, loop_back[h],
                                                   var_count, objpos, arrpos,
                                                   arg_count, args_obj,
                                                   args_arr, whale_loc_obj)) {
                        ru.ok = 1;
                        ru.objpos = objpos;
                        ru.arrpos = arrpos;
                        whale_loop_reuse[h] = ru;
                    }
                }
            }
        }
        /* Int-region summary: enabled only with exactly one recognised loop
           (mirrors are per-function globals; two regions could name the same
           local). The emit pass consults whale_int_mirror/depth, the exit
           edge consults whale_int_exit, the entry guard whale_int_need_guard.
           Zeroed every compilation - a stale mirror is a wrong answer. */
        memset(whale_int_mirror, 0, sizeof(whale_int_mirror));
        whale_int_ok = 0;
        whale_int_head = -1;
        whale_int_exit = -1;
        whale_int_g = -1;
        whale_int_need_guard = 0;
        whale_int_k_arg = -1;
        {
            int nh = 0, fh = -1, hh;

            for (hh = 0; hh <= len; hh++)
                if (whale_loop_intreg[hh].ok) {
                    nh++;
                    fh = hh;
                }
            if (nh == 1 && fh >= 0 && !whale_generic_mode) {
                int d;

                for (d = 0; d < var_count && d < WHALE_JIT_MAX_SLOTS; d++)
                    whale_int_mirror[d] = whale_loop_intreg[fh].mirror[d];
                whale_int_ok = 1;
                whale_int_head = fh;
                whale_int_exit = whale_loop_intreg[fh].exitp;
                whale_int_g = whale_loop_intreg[fh].g;
                whale_int_need_guard = whale_loop_intreg[fh].need_guard;
                whale_int_k_arg = whale_loop_intreg[fh].k_arg;
            }
        }
    }

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
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt_id("JS_WhaleDeoptNote"); wt('(');
    wt_id("void"); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("flag"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt_id("JS_WhaleDeoptWhy"); wt('(');
    wt_id("void"); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("code"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefGetNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(',');
        wk(JS_WK_INT); wt_id("idx"); wt(','); wk(JS_WK_DOUBLE); wt('*'); wt_id("out"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefGetObj"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(',');
        wk(JS_WK_INT); wt_id("idx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    /* Runtime identity guard for a speculatively inlined closure-slot call
       target: does the slot still hold the function the call site was
       compiled against? The expected function travels as a table index
       (JS_WhaleVrExpectAdd, a compile-time call), never as a pointer
       literal - the token emitters have no 64-bit literal. */
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt('*'); wt_id("JS_WhaleVarRefPval"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(',');
        wk(JS_WK_INT); wt_id("idx"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleVarRefIsFn"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_VOID); wt('*'); wt_id("var_refs"); wt(',');
        wk(JS_WK_INT); wt_id("idx"); wt(','); wk(JS_WK_INT); wt_id("expect_slot"); wt(')'); wt(';');
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
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_WhaleNewFloat64"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_DOUBLE); wt_id("d"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_WhaleNewBool"); wt('(');
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
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt_id("JS_WhaleObjSlotUndef"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("k"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt_id("JS_WhaleObjSlotEmptyString"); wt('(');
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
    /* The layout unpack calls this once per column. The prototype was missing, so
       TinyCC declared it implicitly and took the double result for an int - the
       call still linked and happened to be right only because the fields in the
       cases at hand were small integers. Exactly the trap the block below warns
       about; this one slipped through. */
    wk(JS_WK_EXTERN); wk(JS_WK_DOUBLE); wt_id("JS_WhaleObjFieldNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("atom"); wt(',');
        wk(JS_WK_VOID); wt('*'); wt('*'); wt_id("shape_cache"); wt(',');
        wk(JS_WK_VOID); wt('*'); wt('*'); wt_id("ptr_cache"); wt(',');
        wk(JS_WK_INT); wt('*'); wt_id("deopt"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleStructNew"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(',');
        wt_id("const"); wk(JS_WK_INT); wt('*'); wt_id("atoms"); wt(',');
        wk(JS_WK_INT); wt_id("nslots"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_DOUBLE); wt_id("JS_WhaleStructFieldNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("atom"); wt(','); wk(JS_WK_DOUBLE); wt('*'); wt_id("out"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleStructSetNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("atom"); wt(','); wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt('*'); wt_id("JS_WhaleStructDataPtr"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt('*'); wt_id("len_out"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleNativeEnsureSlot"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt('*'); wt_id("atoms"); wt(','); wk(JS_WK_INT); wt_id("n"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_DOUBLE); wt_id("JS_WhaleNativeSlotFieldNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("col"); wt(','); wk(JS_WK_INT); wt('*'); wt_id("deopt"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleNativeSlotPutNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_INT); wt_id("col"); wt(','); wk(JS_WK_INT); wt_id("atom"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wt_id("JSValue"); wt_id("JS_WhaleObjGet"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("k"); wt(')'); wt(';');
    /* Raising. Without this prototype TinyCC declares it implicitly, the JSValue
       argument is passed as an int, and the generated code fails to parse - which
       is what an `implicit declaration of function 'JS_WhaleThrow'` warning next
       to `identifier expected` means. */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleThrow"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt_id("v"); wt(')'); wt(';');
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
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleArrSetVal"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("idx"); wt(','); wk(JS_WK_INT); wt_id("val_slot"); wt(')'); wt(';');
    /* Host calls. Declared like everything else: without a prototype TinyCC
       applies the C89 implicit declaration and a double argument goes to the
       wrong register - the call still links and silently computes nonsense. */
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjSlotNum"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("v"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleArgGetObj"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wt_id("JSValue"); wt_id("v"); wt(',');
        wk(JS_WK_INT); wt_id("slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleArrBind"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wt_id("JSValue"); wt('*'); wt_id("args"); wt(','); wk(JS_WK_INT); wt_id("idx"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleObjSlotCopy"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("src"); wt(',');
        wk(JS_WK_INT); wt_id("dst"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleCallValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("func_slot"); wt(',');
        wk(JS_WK_INT); wt_id("argc"); wt(','); wk(JS_WK_INT); wt_id("arg_base"); wt(',');
        wk(JS_WK_INT); wt_id("ret_slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleConstructValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("func_slot"); wt(',');
        wk(JS_WK_INT); wt_id("argc"); wt(','); wk(JS_WK_INT); wt_id("arg_base"); wt(',');
        wk(JS_WK_INT); wt_id("ret_slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleCallSelfValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(',');
        wk(JS_WK_INT); wt_id("argc"); wt(','); wk(JS_WK_INT); wt_id("arg_base"); wt(',');
        wk(JS_WK_INT); wt_id("ret_slot"); wt(')'); wt(';');
    wk(JS_WK_EXTERN); wk(JS_WK_INT); wt_id("JS_WhaleCallMethodValue"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("func_slot"); wt(',');
        wk(JS_WK_INT); wt_id("this_slot"); wt(','); wk(JS_WK_INT); wt_id("argc"); wt(',');
        wk(JS_WK_INT); wt_id("arg_base"); wt(','); wk(JS_WK_INT); wt_id("ret_slot"); wt(')'); wt(';');
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
    /* memset for whole-loop Uint8Array fills (see whale_loop_memset_emit).
       Same implicit-declaration trap: the pointer return and the size_t
       argument both need a prototype. */
    wk(JS_WK_EXTERN); wk(JS_WK_VOID); wt('*'); wt_id("memset"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("s"); wt(','); wk(JS_WK_INT); wt_id("c"); wt(',');
        wk(JS_WK_UNSIGNED); wk(JS_WK_LONG); wk(JS_WK_LONG); wt_id("n"); wt(')'); wt(';');
    /* nonzero-count for whole-loop `if (arr[g]) acc++` reduction. */
    wk(JS_WK_EXTERN); wk(JS_WK_DOUBLE); wt_id("JS_WhaleTAU8CountNZ"); wt('(');
        wk(JS_WK_VOID); wt('*'); wt_id("ctx"); wt(','); wk(JS_WK_INT); wt_id("slot"); wt(',');
        wk(JS_WK_DOUBLE); wt_id("lo"); wt(','); wk(JS_WK_DOUBLE); wt_id("hi"); wt(')'); wt(';');
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
    /* Closure-slot storage cells, fetched once in the prologue: get_var_ref
       and put_var_ref read and write through them directly. */
    wt_id("JSValue"); wt('*'); wt_id("_p0"); wt(';');
    wt_id("JSValue"); wt('*'); wt_id("_p1"); wt(';');
    wt_id("JSValue"); wt('*'); wt_id("_p2"); wt(';');
    wt_id("JSValue"); wt('*'); wt_id("_p3"); wt(';');
    /* The fresh struct's columns, set by JS_WhaleStructDataPtr right after
       JS_WhaleStructNew. Valid only while whale_str_fresh names this slot -
       no call, no jump target in between - which is exactly the run of
       initialiser stores a factory body is. */
    wt_id("JSValue"); wt('*'); wt_id("_sdp"); wt(';');
    /* The locals. The two hottest numeric locals in a single call-free loop
       are pinned (write-through, so the home slot stays authoritative - see
       the ELS note in tccgen.c): loop-carried values then ride in xmm across
       the whole loop instead of crossing the memory bus twice per trip.
       The pool holds 2 (TCC's expression evaluation needs the rest); the pin
       decision is heat order, object and single-assignment locals excluded.
       _wv pins below compete for the same pool and lose (plain doubles when
       the pool is full) - loop-carried values outrank fold temporaries. */
    {
        int pin0 = -1, pin1 = -1;

        if (info->loop_count == 1 && !whale_loop_has_call) {
            int d, h0 = 0, h1 = 0;

            for (d = 0; d < var_count && d < WHALE_JIT_MAX_SLOTS; d++) {
                int h;

                if (d < WHALE_JIT_MAX_SLOTS && whale_loc_obj[d])
                    continue;
                if (d < WHALE_JIT_MAX_SLOTS && whale_lo_c[d])
                    continue;
                h = whale_loop_heat[d];
                if (h > h0) {
                    h1 = h0;
                    pin1 = pin0;
                    h0 = h;
                    pin0 = d;
                } else if (h > h1) {
                    h1 = h;
                    pin1 = d;
                }
            }
            if (h0 < 3)
                pin0 = -1;
            if (h1 < 3)
                pin1 = -1;
        }
        for (i = 0; i < var_count; i++) {
            if (i == pin0 || i == pin1)
                wk(JS_WK_REGISTER);
            wk(JS_WK_DOUBLE); wt_slot("L", i); wt('='); wt_int(0); wt(';');
        }
    }
    /* Int-region mirrors: one C `int` per mirrored local, zero-initialised
       with the rest. Only when exactly one int-region was recognised (the
       whale_int_ok gate above); otherwise nothing names them and the stores
       would be pure per-call tax. `I<d>` is free (no other prefix uses it). */
    if (whale_int_ok) {
        for (i = 0; i < var_count && i < WHALE_JIT_MAX_SLOTS; i++) {
            if (whale_int_mirror[i]) {
                wk(JS_WK_INT); wt_slot("I", i); wt('='); wt_int(0); wt(';');
            }
        }
    }
    /* ELS spill slots: `register double` locals for the hottest materialised
       values inside loops. Declared ONLY when they can be used - a function
       with no loops (recursive fib) or with calls in its loops must not pay
       8 zero-init stores per call for slots nothing will ever name; nested
       loops are also out. Only TWO carry `register`: the pin pool is 4 deep
       and TinyCC's expression evaluation needs spare float registers - pinning
       all four exhausted the pool (get_reg -1) and the loop hung. The rest
       are plain doubles. Conditions match fold_materialize()'s assignment. */
    if (info->loop_count == 1 && !whale_loop_has_call) {
        for (i = 0; i < WHALE_VPIN_MAX; i++) {
            if (i < 2)
                wk(JS_WK_REGISTER);
            wk(JS_WK_DOUBLE); wt_slot("_wv", i); wt('='); wt_int(0); wt(';');
        }
    }
    if (whale_dbg_env("WHALE_STR_DBG")) {
        int cols = whale_layout_from_sites(whale_field_sites, whale_field_sites_n,
                                           whale_str_atoms, WHALE_ATOM_MAX,
                                           info->prefer_gcc ? 0 : 40);
        int si;

        fprintf(stderr, "[str-table] blen=%d sites=%d mono=%d cols=%d atoms=[",
                info->byte_code_len, whale_field_sites_n,
                whale_sites_mono(whale_field_sites, whale_field_sites_n), cols);
        for (si = 0; si < cols; si++)
            fprintf(stderr, "%s%s", si ? "," : "",
                    JS_AtomToCString(ctx, whale_str_atoms[si]));
        fprintf(stderr, "] site_pos=[");
        for (si = 0; si < whale_field_sites_n; si++)
            fprintf(stderr, "%s%d", si ? "," : "", whale_field_sites[si]);
        fprintf(stderr, "]\n");
    }
    /* The struct-column variables. Declared only when the function can use
       them (field accesses or object-holding locals, both known from the
       pre-scan): a pure numeric function like recursive fib must not pay 32
       zero-init stores per call for columns nothing will ever name - at
       ~240M calls (fib(26)x20) that was the 7ms-to-20ms gap. The gate covers
       every use (unboxed-struct paths all require a field site or an object
       local first), so no `Lundeclared` divergence like vnode_build's. */
    {
        int need_cols = (whale_field_sites_n > 0);
        int d;

        if (!need_cols) {
            for (d = 0; d < var_count && d < WHALE_JIT_MAX_SLOTS; d++)
                if (whale_loc_obj[d]) {
                    need_cols = 1;
                    break;
                }
        }
        for (i = 0; i < WHALE_ATOM_MAX; i++) {
            whale_str_L[i] = var_count + i;
            if (need_cols) {
                wk(JS_WK_DOUBLE); wt_slot("L", whale_str_L[i]); wt('='); wt_int(0); wt(';');
            }
        }
    }
    /* All of the generator's scratch state in one zero-initialised block: the
       value slots (stack_size plus the spare names fold_materialize() hands out,
       see whale_vname) and the per-slot caches. Declaring these as individual
       variables cost one statement each, and TinyCC spills at every statement -
       a short function spent nearly all of its time in its own prologue. */
    /* Value temporaries: a bare array, deliberately NOT zero-initialised
       (every slot is assigned before it is read - see wt_arr). */
    wk(JS_WK_DOUBLE); wt_id("_wsv"); wt('['); wt_int(stack_size + WHALE_VSER_MAX); wt(']'); wt(';');
    /* The rest lives in one zero-initialised block, but only the groups this
       function can touch: field caches (object work), call slots (calls),
       TAU8 caches (array access). Each group is load-bearing-zero only when
       present (fv[] == 0 means "uncached"), so absent groups simply vanish -
       a pure numeric function like fib keeps nothing but _wsv[]. */
    {
        int need_field = 0, d;
        int need_reuse = 0, hh;

        need_field = (whale_field_sites_n > 0);
        if (!need_field) {
            for (d = 0; d < var_count && d < WHALE_JIT_MAX_SLOTS; d++)
                if (whale_loc_obj[d]) {
                    need_field = 1;
                    break;
                }
        }
        for (hh = 0; hh <= len; hh++)
            if (whale_loop_reuse[hh].ok) {
                need_reuse = 1;
                break;
            }
        if (need_field || whale_has_array || need_reuse) {
            wk(JS_WK_STRUCT); wt('{');
            if (need_field) {
                /* the atoms of the layout being built, handed to JS_WhaleStructNew */
                wk(JS_WK_INT); wt_id("wa"); wt('['); wt_int(WHALE_ATOM_MAX); wt(']'); wt(';');
                wk(JS_WK_VOID); wt('*'); wt_id("fs"); wt('['); wt_int(WHALE_FIELD_CACHE); wt(']'); wt(';');
                wt_id("JSValue"); wt('*'); wt_id("fp"); wt('['); wt_int(WHALE_FIELD_CACHE); wt(']'); wt(';');
                wk(JS_WK_INT); wt_id("fver"); wt(';');
                wk(JS_WK_INT); wt_id("fv"); wt('['); wt_int(WHALE_FIELD_CACHE); wt(']'); wt(';');
                /* unboxed struct presence flags, one per layout slot */
                wk(JS_WK_UNSIGNED); wt_id("char"); wt_id("sp"); wt('['); wt_int(WHALE_STRUCT_SLOTS * WHALE_ATOM_MAX); wt(']'); wt(';');
            }
            /* slot-cache generation: cleared whenever a local takes an
               object/array value (the slot may hold an array next) and read
               by the TAU8 resolve, so it belongs to either group. */
            wk(JS_WK_INT); wt_id("oi"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
            if (whale_has_array) {
                wt_id("JSValue"); wt('*'); wt_id("ap"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
                wk(JS_WK_DOUBLE); wt_id("an"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
                wk(JS_WK_INT); wt_id("ai"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
                wk(JS_WK_UNSIGNED); wt_id("char"); wt('*'); wt_id("op"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
                wk(JS_WK_DOUBLE); wt_id("on"); wt('['); wt_int(WHALE_OBJ_SLOTS); wt(']'); wt(';');
            }
            /* in-loop object-reuse marks, one per reused build (4 cap): 0 means
               "not built yet this call", so the first trip builds and later
               trips only store values. Inside the zeroed _wsc like the rest. */
            if (need_reuse) {
                wk(JS_WK_INT); wt_id("ru"); wt('['); wt_int(4); wt(']'); wt(';');
            }
            wt('}'); wt_id("_wsc"); wt('='); wt('{'); wt_int(0); wt('}'); wt(';');
            /* 1, because a cached fv[] of 0 (from that zero initialiser) must not look
               like the current generation. */
            if (need_field) {
                wt_id("_wsc"); wt('.'); wt_id("fver"); wt('='); wt_int(1); wt(';');
            }
        }
    }
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
    /* Closure slots this reading reads as values: parked once here, in the
       structural prologue. The captured value cannot change during the call -
       the same fact the entry's self-check relies on - so re-parking per read
       was one helper call and one dup per loop iteration for nothing. */
    for (i = 0; i < 4; i++)
        whale_vr_slot[i] = -1;
    for (i = 0; i < 4; i++) {
        if (!var_ref_val[i])
            continue;
        if (whale_objs >= WHALE_CALL_BASE) {
            whale_objs = WHALE_CALL_BASE;   /* no room: refuse the function */
            { whale_out_line = __LINE__; goto out; }
        }
        whale_vr_slot[i] = whale_objs++;
        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleVarRefGetObj"); wt('(');
        wt_id("ctx"); wt(','); wt_id("var_refs"); wt(','); wt_int(i); wt(',');
        wt_int(whale_vr_slot[i]); wt(')'); wt(')');
        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    }
    /* Closure slots read or written as values: fetch the storage cell once.
       get_var_ref / put_var_ref then run as tag-checked loads and stores
       through the pointer - `n = n + 1` inside a compiled closure loop costs
       a load, an add and a store, with no helper call in sight. */
    for (i = 0; i < 4; i++) {
        if (!var_ref_rw[i])
            continue;
        wk(JS_WK_IF); wt('('); wt('!'); wt('('); wt_slot("_p", i); wt('=');
        wt_id("JS_WhaleVarRefPval"); wt('('); wt_id("ctx"); wt(','); wt_id("var_refs"); wt(',');
        wt_int(i); wt(')'); wt(')'); wt(')'); wt('{');
        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
    }
    fold_commit(0);     /* the whole implicit prologue is structural */

    for (i = 0; i < var_count; i++)
        loc_slot[i] = -1;

    /* Slots for the arguments that travel as objects. Handed out before the body
       is generated so the entry unboxing and every `get_arg` agree on the number,
       and (like every other slot) numbered below the call scratch area. */
    for (i = 0; i < arg_count; i++) {
        arg_obj_slot[i] = -1;
        if (whale_dbg_env("WHALE_ARG_DBG")) fprintf(stderr, "[argslot] i=%d args_obj=%d objs=%d argc=%d varc=%d len=%d\n", i, args_obj[i], whale_objs, arg_count, var_count, len);
        if (args_obj[i] || args_arr[i]) {
            if (whale_objs >= WHALE_CALL_BASE) {
                whale_objs = WHALE_CALL_BASE;   /* no room: refuse the function */
                { whale_out_line = __LINE__; goto out; }
            }
            arg_obj_slot[i] = whale_objs++;
        }
    }

    /* How long an expression may grow before it is forced into a variable.

       It was tempting to raise this for functions with no object work, on the
       theory that they have no pinned registers to compete with. Measured: 32
       against 24 gives bitops 143ms against 157, but nested 92 against 84 and
       locals 104 against 98 - a wash, and 48 is worse than both. TinyCC spills at
       every statement, so a longer expression is not free. The ceiling stays where
       it was measured. */
    whale_expr_max = 24;
#ifdef WHALE_DEBUG
    if (getenv("WHALE_EXPR_MAX"))
        whale_expr_max = atoi(getenv("WHALE_EXPR_MAX"));
#endif
    if (whale_dbg_env("WHALE_EXPR_DBG"))
        fprintf(stderr, "[expr] blen=%d sites=%d call=%d max=%d\n",
                info->byte_code_len, whale_field_sites_n, whale_has_call,
                whale_expr_max);

    sp = 0;
    if (whale_dbg_env("WHALE_OP_DBG")) {
        int k;

        fprintf(stderr, "[targets] len=%d:", len);
        for (k = 0; k < len; k++)
            if (targets[k])
                fprintf(stderr, " %d", k);
        fprintf(stderr, "\n");
    }
    for (pos = 0; pos < len; ) {
        int op = pc[pos];
        int sz = whale_op_size_of(op);
        int ta, tb, t;
        int k1 = 0, i1 = 0, k2 = 0, i2 = 0;
        int l1 = 0, l2 = 0, ok1, ok2 = 0, p2 = 0;
        int t1 = 0, t2 = 0;
        int c1 = 0, c2 = 0;
        double cv1 = 0, cv2 = 0;

        /* Int-region entry guard, emitted once before the first opcode: a
           parameter bound is unproven, and `I<g>++` past INT_MAX is UB while
           JS would carry on as a double. One test per call (never in the
           loop), and on the cold path the whole function deopts to the
           interpreter - which is where an unproven function would have run
           anyway. Literal bounds were capped at 2^30 during recognition, so
           they need no guard. */
        if (pos == 0 && whale_int_ok && whale_int_need_guard) {
            int fm = fold_mark();

            wk(JS_WK_IF); wt('(');
            wt_id("An"); wt('['); wt_int(whale_int_k_arg); wt(']');
            wt_op(JS_OP_GT); wt_num(1073741824.0);
            wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
            fold_commit(fm);
        }

        /* A loop head opens a buffer for its body; the back edge closes it.
           The head label goes inside the buffer, because that is where the back
           edge jumps to. Nesting uses the loop stack. */
        if (loop_back[pos] >= 0) {
            loop_begin(pos, loop_back[pos]);
            /* A new loop body starts: no presence flag is proven on entry -
               the back edge re-runs the body, and a flag proven on one trip
               says nothing about the next. Set after loop_begin, which is
               what fixes the depth. */
            whale_sp_reset();
        }

        /* dead code after an unconditional return/goto is not emitted */
        if (unreachable && !targets[pos]) {
            pos += sz;
            continue;
        }
        if (targets[pos])
            unreachable = 0;

        /* A basic-block boundary: the value of every deferred local has to be in
           its variable again, because the code that reaches this point may not
           be the code that set the pending expression. A join also ends the
           straight-line segment the presence-flag dedup reasons about: a flag
           proven on one incoming edge is not proven on the other. */
        if (targets[pos]) {
            local_flush_all(var_count);
            whale_sp_reset();
            /* Int-region exit boxing: the loop is left through pos, so every
               mirror is boxed back once (`L<d> = (double)I<d>`). The flush
               above already wrote pending expressions to I<d> (materialise
               honours mirrors inside the loop); this runs after it, on real
               variables only. It fires exactly at the head test's exit - a
               body-internal join shares targets[] but never equals exitp
               (recognition refused exits inside [head, back]). */
            if (whale_int_ok && pos == whale_int_exit) {
                int dd;

                for (dd = 0; dd < var_count && dd < WHALE_JIT_MAX_SLOTS; dd++) {
                    if (whale_int_mirror[dd]) {
                        int fm = fold_mark();

                        wt_slot("L", dd); wt('=');
                        wt('('); wk(JS_WK_DOUBLE); wt(')');
                        wt_slot("I", dd); wt(';');
                        fold_commit(fm);
                    }
                }
            }
        }

        /* The fresh-struct window closes at any boundary a jump can reach or
           any call: past either, the object in the slot may not be the one
           JS_WhaleStructDataPtr resolved. */
        if (whale_str_fresh >= 0 && (targets[pos] || whale_is_call_op(pc[pos])))
            whale_str_fresh = -1;

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
                       `if`, I32 down the other) lost bitops to the interpreter.
                       IBIT rides the same arms: a bare-int hint merges down too. */
                    if (sv == WHALE_T_I32 || sv == WHALE_T_ICONST ||
                        sv == WHALE_T_IBIT) {
                        if (whale_type_is_num(cv)) {
                            snap[base + k] = WHALE_T_NUM;
                            continue;
                        }
                    }
                    if (cv == WHALE_T_I32 || cv == WHALE_T_ICONST ||
                        cv == WHALE_T_IBIT) {
                        if (whale_type_is_num(sv)) {
                            snap[base + k] = WHALE_T_NUM;
                            continue;
                        }
                    }
                    /* Two object-like types in the very same slot: the object is
                       the one the slot holds either way, and the only thing that
                       can disagree is whether the layout columns still describe it.
                       A struct on one edge means they were read from the object
                       that edge had; a plain value on the other means a new object
                       was stored over it. The columns are stale then, so the merge
                       has to keep the non-struct reading - which is exactly the
                       "fall back to the map" path, and correct.

                       This is the shape of `st = setField(st, i % 11)`: the entry
                       edge lays `st` out in slot 0 (`var st = {a:0,...}`), the back
                       edge stores setField's result in the same slot, and the two
                       meet as STRUCT(0) against VAL(0). Treating that as a conflict
                       is what refused `step` - twice over, before the object slots
                       were unified and after. */
                    if (WHALE_IS_OBJLIKE(sv) && WHALE_IS_OBJLIKE(cv) &&
                        WHALE_SLOT(sv) == WHALE_SLOT(cv)) {
                        /* At a loop head the snapshot holds the entry edge and
                           cur the back edge: the back edge's store is newer, so
                           keep it (this is the setField shape above). Anywhere
                           else (a branch join inside the body) both edges are
                           new values in different representations - columns on
                           one side, slot identity on the other - and neither
                           reading is correct for the other edge: refuse. That
                           shape (e.g. `o = o1` filling columns on one branch,
                           `o = o2` copying the slot on the other) silently read
                           the stale slot before. */
                        if (loop_back[pos] >= 0) {
                            if (WHALE_IS_STRUCT(sv) && !WHALE_IS_STRUCT(cv))
                                snap[base + k] = cv;
                            else if (!WHALE_IS_STRUCT(sv) && WHALE_IS_STRUCT(cv))
                                snap[base + k] = sv;
                        } else if ((WHALE_IS_STRUCT(sv) || WHALE_IS_STRUCT(cv)) &&
                                   sv != cv) {
                            if (whale_dbg_env("WHALE_MERGE_DBG"))
                                fprintf(stderr, "[merge-obj-refuse] pos=%d slot=%d prev=%d now=%d\n",
                                        pos, k, sv, cv);
                            differ = 1;
                            break;
                        }
                        if (whale_dbg_env("WHALE_MERGE_DBG"))
                            fprintf(stderr, "[merge-obj] pos=%d slot=%d prev=%d now=%d -> %d\n",
                                    pos, k, sv, cv, snap[base + k]);
                        continue;
                    }
                    if (sv == WHALE_T_UNKNOWN) {
                        snap[base + k] = cv; /* optimistic merge */
                    } else if (cv != WHALE_T_UNKNOWN) {
                        if (whale_dbg_env("WHALE_MERGE_DBG"))
                            fprintf(stderr, "[merge] pos=%d slot=%d (of %d) "
                                    "prev=%d now=%d\n", pos, k, live, sv, cv);
                        differ = 1;
                        break;
                    }
                }
                if (differ)
                    { whale_out_line = __LINE__; goto out; }
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
               to the block that just ended. Slots that carry a value - an object, an
               array, a parked value, a laid-out struct - are skipped: their "type"
               is the slot number, there is no expression to materialise, and asking
               for one emits `v[k] = () ;`, which TinyCC refuses. That is exactly the
               shape a `catch` produces, since it parks undefined where the handler
               will look for the exception. */
            {
                int k;

                for (k = 0; k < sp; k++) {
                    int tk = cur[var_count + k];

                    if (WHALE_IS_SLOT(tk) || WHALE_IS_STRUCT(tk))
                        continue;
                    fold_materialize(k);
                }
            }
            {
                int fm = fold_mark();

                if (whale_dbg_env("WHALE_OP_DBG"))
                    fprintf(stderr, "[label] pos=%d depth=%d outlen=%d\n",
                            pos, whale_loop_depth, *whale_out_len);
                wt_slot("L", pos); wt(':');
                fold_commit(fm);
            }
        }

        /* --- peephole 1: <load> <load> <binop> [put_loc] -> one assignment --- */
        ok1 = whale_load_analyze(info, pc, pos, cur, var_count, whale_lo_c, whale_lo_cv, &l1, &t1, &k1, &i1, &cv1);
        if (ok1) {
            p2 = pos + l1;
            if (p2 < len)
                ok2 = whale_load_analyze(info, pc, p2, cur, var_count, whale_lo_c, whale_lo_cv, &l2, &t2, &k2, &i2, &cv2);
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
                            /* The result IS the kept operand, so its
                               invariance travels with it. */
                            fold_set(sp, fm, id_keep == 0
                                     ? whale_load_inv(k1, i1)
                                     : whale_load_inv(k2, i2));
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
                        int btgt = 0, bis_true = 0, bisz = 0;

                        /* Same compare-into-branch as the general path, for the
                           peephole shape: `<load> <load> <cmp>; if` folds to one
                           branch with no boolean temporary. State machine
                           mirrors the unfused push-plus-if (a BOOL slot is
                           recorded and then consumed): only the emitted shape
                           differs. */
                        if (whale_next_is_branch(pc, len, p4 + l4, 0, &btgt, &bis_true, &bisz)) {
                            int fm, cf;

                            local_flush_all(var_count);
                            fm = fold_mark();
                            wt('('); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(')');
                            fold_set(sp, fm, whale_load_inv(k1, i1) && whale_load_inv(k2, i2));
                            cur[var_count + sp] = WHALE_T_BOOL;
                            sp += 1;
                            cf = fold_mark();
                            wk(JS_WK_IF); wt('(');
                            if (!bis_true)
                                wt('!');
                            wt('('); fold_inline(sp - 1); wt(')');
                            wt(')'); wk(JS_WK_GOTO); wt_slot("L", btgt); wt(';');
                            fold_commit(cf);
                            sp--;
                            if ((btgt >= 0) && (btgt <= len) && (!seen[btgt])) {
                                cur[state_len - 1] = (uint8_t)sp;
                                seen[btgt] = 1;
                                memcpy(snap + (size_t)btgt * state_len, cur, state_len);
                            }
                            pos = p4 + l4 + bisz;
                            continue;
                        }
                        {
                            int fm = fold_mark();

                            wt('('); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0);
                            fold_set(sp, fm, whale_load_inv(k1, i1) && whale_load_inv(k2, i2));
                            cur[var_count + sp] = WHALE_T_BOOL;
                            sp += 1;
                        }
                    }
                } else {
                    /* Constant folding: both sides are literals, so evaluate
                       now and emit the answer. Double arithmetic matches the
                       runtime exactly (same IEEE semantics, including 1/0=Inf
                       and 0/0=NaN), and there is no int-divide trap because the
                       computation stays in doubles. This is what turns
                       loop-invariant `(a+b)` with constant a/b into `4.0`. */
                    if (!is_cmp &&
                        (k1 == WLD_INT || k1 == WLD_REAL) &&
                        (k2 == WLD_INT || k2 == WLD_REAL) &&
                        (opid == JS_OP_ADD || opid == JS_OP_SUB ||
                         opid == JS_OP_MUL || opid == JS_OP_DIV)) {
                        double cvr = (opid == JS_OP_ADD) ? cv1 + cv2
                            : (opid == JS_OP_SUB) ? cv1 - cv2
                            : (opid == JS_OP_MUL) ? cv1 * cv2 : cv1 / cv2;
                        int is_int = (cvr == (double)(int)cvr &&
                                      cvr >= -2147483648.0 &&
                                      cvr <= 2147483647.0);
                        int fm = fold_mark();

                        if (dst >= 0) {
                            local_forget(dst);
                            wt_slot("L", dst); wt('=');
                            if (is_int)
                                wt_int((int)cvr);
                            else
                                wt_num(cvr);
                            wt(';');
                            fold_commit(fm);
                            cur[dst] = WHALE_T_NUM;
                            if (assign_cnt[dst] == 1) {
                                whale_lo_c[dst] = 1;
                                whale_lo_cv[dst] = cvr;
                            } else {
                                whale_lo_c[dst] = 0;
                            }
                        } else {
                            if (is_int)
                                wt_int((int)cvr);
                            else
                                wt_num(cvr);
                            fold_set(sp, fm, 1);   /* folded literals */
                            cur[var_count + sp] = WHALE_T_NUM;
                            sp += 1;
                        }
                        pos = p4 + l4;
                        continue;
                    }
                    /* inv tracks loop-invariance through the peephole. A SLOT
                       operand whose local the loop body never writes is as
                       constant as a literal - `a` in
                       `for (...) s = (a+b)*...` - so whale_load_inv counts
                       INT/REAL/ARG and unwritten SLOTs alike. */
                    {
                        int inv1 = whale_load_inv(k1, i1);
                        int inv2 = whale_load_inv(k2, i2);

                        if (dst >= 0) {
                            int fm = fold_mark();

                            local_forget(dst);
                            wt_slot("L", dst); wt('='); whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2); wt(';');
                            fold_commit(fm);
                            cur[dst] = WHALE_T_NUM;
                        } else {
                            int fm = fold_mark();

                            whale_load_emit(k1, i1, cv1); wt_op(opid); whale_load_emit(k2, i2, cv2);
                            fold_set(sp, fm, inv1 && inv2);
                            cur[var_count + sp] = WHALE_T_NUM;
                            sp += 1;
                        }
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
                /* c1 was never assigned (it stays 0 from its declaration), so
                   this arm never fired and constant initialisers never
                   propagated - every loop re-read them as variables. A load
                   whose kind is a constant IS the constant. */
                if (assign_cnt[d2] == 1 &&
                    (k1 == WLD_INT || k1 == WLD_REAL)) {
                    whale_lo_c[d2] = 1;
                    whale_lo_cv[d2] = cv1;
                } else {
                    whale_lo_c[d2] = 0;
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
                fold_set(sp, fm, 1);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;
        case OP_push_i8:
            {
                int fm = fold_mark();

                wt_int((int)(int8_t)pc[pos + 1]);
                fold_set(sp, fm, 1);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;
        case OP_push_i16:
            {
                int fm = fold_mark();

                wt_int((int)(int16_t)whale_get_u16(pc + pos + 1));
                fold_set(sp, fm, 1);
                cur[var_count + sp] = WHALE_T_ICONST;
                sp++;
            }
            break;
        case OP_push_i32:
            {
                int fm = fold_mark();

                wt_int((int)(int32_t)whale_get_u32(pc + pos + 1));
                fold_set(sp, fm, 1);
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                c = info->cpool[idx];
                ct = JS_VALUE_GET_TAG(c);
                if (ct != JS_TAG_INT && !JS_TAG_IS_FLOAT64(ct))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); } /* strings / bigints / objects: not our shape */
                fm = fold_mark();
                if (ct == JS_TAG_INT)
                    wt_int((int)JS_VALUE_GET_INT(c));
                else
                    wt_num(JS_VALUE_GET_FLOAT64(c));
                fold_set(sp, fm, 1);
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;
        case OP_push_true:
            {
                int fm = fold_mark();

                wt_int(1);
                fold_set(sp, fm, 1);
                cur[var_count + sp] = WHALE_T_BOOL;
                sp++;
            }
            break;
        case OP_push_false:
            {
                int fm = fold_mark();

                wt_int(0);
                fold_set(sp, fm, 1);
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
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                fold_set(sp - 1, fm, 1);
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

        /* `new F(...)`. Stack is [func, new.target, args...].

           `new Uint8Array(n)` has its own lowering. Everything else goes through the
           engine's constructor path: `new` does the whole prototype dance, and one
           call out is the honest price for keeping the function compiling at all -
           a single `throw new Error(...)` used to drop it back to the interpreter,
           because this opcode was refused and the refusal was global. */
        case OP_call_constructor:
            {
                int argc = (int)whale_get_u16(pc + pos + 1);
                int fslot = sp - argc - 2;
                int tc;

                if (fslot < 0 || argc < 0 || argc > WHALE_CALL_SLOTS)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                tc = cur[var_count + fslot];
                if (argc == 1 && WHALE_IS_OBJ(tc) &&
                    whale_type_is_num(cur[var_count + sp - 1])) {
                    /* new.target is deliberately not checked: for `new Uint8Array(n)`
                       the bytecode passes the constructor itself as the target, and a
                       target that is some other function cannot change what this
                       construction makes. */
                    int out = whale_objs++;

                    if (out >= WHALE_OBJ_SLOTS)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    {
                        int fm = fold_mark();

                        wk(JS_WK_IF); wt('('); wt('!');
                        wt_id("JS_WhaleNewU8Array"); wt('('); wt_id("ctx"); wt(',');
                        wt_int(WHALE_OBJ_SLOT(tc)); wt(',');
                        fold_inline(sp - 1); wt(','); wt_int(out); wt(')');
                        wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    sp = fslot;
                    cur[var_count + sp] = WHALE_OBJ_T(out);
                    sp++;
                    break;
                }
                {
                    int cc = -1, ret_slot, k, fm, nxt = pos + sz;

                    /* The callee has to be reachable through a slot: the helper looks
                       it up there. A function this compilation can see is parked from
                       the constant pool; anything else must already be parked. */
                    if (whale_dbg_env("WHALE_JIT_DUMP"))
                        fprintf(stderr, "[ctor] pos=%d argc=%d tc=%d fslot=%d sp=%d\n",
                                pos, argc, tc, fslot, sp);
                    if (WHALE_IS_VAL(tc) || WHALE_IS_OBJ(tc))
                        cc = WHALE_IS_VAL(tc) ? WHALE_VAL_SLOT(tc) : WHALE_OBJ_SLOT(tc);
                    else if (tc != WHALE_T_FN)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    for (k = 0; k < argc; k++) {
                        int tk = cur[var_count + sp - argc + k];

                        if (!whale_type_is_num(tk) && !WHALE_IS_VAL(tk) && !WHALE_IS_OBJ(tk))
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    }
                    /* Same rule as a general call: the result is either dead or it is
                       parked as an opaque value. A number would deopt later, and that
                       would run the constructor - and its side effects - twice. */
                    if (nxt < len && (int)pc[nxt] == OP_drop) {
                        ret_slot = -1;
                    } else {
                        if (whale_objs >= WHALE_CALL_BASE)
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
                        ret_slot = whale_objs++;
                    }
                    fm = fold_mark();
                    if (cc < 0) {
                        if (whale_objs >= WHALE_CALL_BASE)
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
                        cc = whale_objs++;
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleCpoolSlot"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(whale_slot_fn[fslot]); wt(',');
                        wt_int(cc); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    }
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
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleConstructValue"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(cc); wt(','); wt_int(argc); wt(',');
                    wt_int(WHALE_CALL_BASE); wt(','); wt_int(ret_slot); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    sp = fslot;
                    cur[var_count + sp] = (ret_slot < 0)
                        ? (uint8_t)WHALE_T_UNKNOWN : WHALE_VAL_T(ret_slot);
                    sp++;
                }
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
                        { whale_out_line = __LINE__; goto out; }
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
                        whale_vexpr[sp - 2].inv = 0;   /* a memory read */
                    }
                    cur[var_count + sp - 2] = WHALE_T_NUM;
                    sp--;
                    break;
                }
                if (!WHALE_IS_SLOT(ta) || !whale_type_is_num(tb))
                    { whale_out_line = __LINE__; goto out; }
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
                    whale_vexpr[sp - 2].inv = 0;   /* a memory read */
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
                int mono;

                /* Was every field access site in this function observed holding a
                   narrow-enough shape set? If so, the objects built here have a
                   settled layout and can be laid out as a struct. Frequency, not
                   unanimity: mildly polymorphic sites still lay out, their rare
                   fields falling back to the dynamic map.

                   Asked at the function level because per-object would need a
                   provenance array kept in step with cur[] and snapshotted at every
                   jump target. Both cases with a measurement behind them build
                   exactly one object, so this covers them; a function with two
                   objects of different shapes is refused, which is the conservative
                   direction.

                   The reservation rule applies inside: the layout is the union of the
                   observed shapes, so a field only some of them carry still gets a
                   slot, because reserving eight bytes beats falling back. */
                /* generic mode never lays out (forces generic objects, whose
                   field access is all resolve). */
                mono = whale_generic_mode ? 0
                    : whale_sites_freq(whale_field_sites, whale_field_sites_n);
                if (whale_dbg_env("WHALE_PROF_DBG"))
                    fprintf(stderr, "[obj] pos=%d sites=%d mono=%d\n",
                            pos, whale_field_sites_n, mono);

                if (whale_objs >= WHALE_CALL_BASE)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                /* A function that hands an object back builds a real one: a
                   JS_CLASS_WHALE_STRUCT with its own columns. It leaves this compilation
                   and the next one meets it as it is - nothing is materialised on the way
                   out or rebuilt on the way in. A function that keeps its object to
                   itself stays on the frame-column path below, which allocates nothing
                   and is already fast. */
                if (!whale_dbg_env("WHALE_NO_UNBOX") && whale_str_n == 0 &&
                    whale_ret_obj_g) {
                    /* The layout: the literal's own define_field names when the
                       profiler saw no access sites (a factory body has none),
                       otherwise the union the sites observed. */
                    int fn = whale_def_atoms_n > 0
                        ? whale_def_atoms_n
                        : whale_layout_from_sites(whale_field_sites,
                                                  whale_field_sites_n,
                                                  whale_str_atoms,
                                                  WHALE_ATOM_MAX,
                                                  info->prefer_gcc ? 0 : 40);

                    if (fn > 0) {
                        int k;

                        if (whale_def_atoms_n > 0)
                            for (k = 0; k < fn; k++)
                                whale_str_atoms[k] = whale_def_atoms[k];
                        {
                            int k, wfm = fold_mark();

                        /* The offset map has to be named before it is built, and
                           these four statements have to be *committed*: the emitters
                           push into the fold sink, and only fold_commit() moves a
                           range into the output. Taking the mark after them (as this
                           did) left them in the sink for good - the generated code
                           built its offset map from `_ws.wa` while `_ws.wa` was
                           still all zeros, so every field lookup by column failed
                           and every field read came back 0. `setField` in
                           immutable_update answered 0 for exactly that. */
                        for (k = 0; k < fn; k++) {
                            wt_arr("wa", k); wt('='); wt_int(whale_str_atoms[k]); wt(';');
                        }
                        fold_commit(wfm);
                        {
                            int afm = fold_mark();

                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStructNew");
                            wt('('); wt_id("ctx"); wt(',');
                            wt_id("_wsc"); wt('.'); wt_id("wa"); wt(',');
                            wt_int(fn); wt(','); wt_int(os); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                            /* The initialiser stores that follow write the
                               columns as plain memory through the data
                               pointer - one resolve instead of one helper
                               (and one column scan) per field. */
                            wt_id("_sdp"); wt('='); wt_id("JS_WhaleStructDataPtr");
                            wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(','); wt_int(0); wt(')'); wt(';');
                            fold_commit(afm);
                            whale_str_fresh = os;
                            whale_str_fresh_n = fn;
                            for (k = 0; k < fn; k++)
                                whale_str_fresh_atoms[k] = whale_str_atoms[k];
                        }
                        }
                        cur[var_count + sp] = WHALE_OBJ_T(os);
                        sp++;
                        break;
                    }
                }
                if (!whale_dbg_env("WHALE_NO_UNBOX") && mono && whale_str_n == 0 &&
                    (whale_call_n == 0 || pos > whale_call_pos[whale_call_n - 1])) {
                    int fn = whale_layout_from_sites(whale_field_sites,
                                                     whale_field_sites_n,
                                                     whale_str_atoms,
                                                     WHALE_ATOM_MAX,
                                                     info->prefer_gcc ? 0 : 40);

                    if (fn > 0) {
                        int k, pfm = fold_mark();

                        for (k = 0; k < fn; k++) {
                            /* sp is the presence flag: a field nobody assigned must
                               not read as a number. The columns themselves were
                               declared and zeroed in the prologue. */
                            wt_arr("sp", k); wt('='); wt_int(0); wt(';');
                            wt_slot("L", whale_str_L[k]); wt('='); wt_int(0); wt(';');
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
                    int ridx = -1;
                    int hd = (whale_loop_depth > 0)
                        ? whale_loops[whale_loop_depth - 1].head_pos : -1;

                    /* In-loop object reuse: when this OP_object is the
                       recognised build of a reuse loop, the slot is reused
                       across trips - first trip builds, later trips skip
                       (the object is still there from the last trip). The
                       ridx maps the loop head to a _wsc.ru[] mark (4 cap;
                       more reuse loops keep per-trip allocation). */
                    if (whale_loop_depth > 0) {
                        if (hd >= 0 && hd <= len && whale_loop_reuse[hd].ok &&
                            whale_loop_reuse[hd].objpos == pos)
                            ridx = whale_reuse_ridx(hd);
                    }
                    if (ridx >= 0) {
                        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ru", ridx); wt(')'); wt('{');
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjNew"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        wt_arr("ru", ridx); wt('='); wt_int(1); wt(';');
                        wt('}');
                    } else {
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjNew"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(os); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    }
                    fold_commit(fm);
                }
                whale_plain_slots |= 1u << os;
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
        case OP_null: case OP_push_empty_string:
            {
                int os;

                if (whale_objs >= WHALE_CALL_BASE)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                os = whale_objs++;
                {
                    int fm = fold_mark();

                    /* The empty string has no operand to name an atom, so it has its
                       own helper; everything else about the two is the same. */
                    wt_id(op == OP_null ? "JS_WhaleObjSlotNull"
                                        : "JS_WhaleObjSlotEmptyString");
                    wt('('); wt_id("ctx"); wt(',');
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
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

                /* A real (C-struct) object keeps its fields in its own columns, so a
                   literal's initialiser is a store into a column. The helper finds the
                   column from the object's offset map, and the write goes through to the
                   property as well, so the interpreter sees the same value. */
                if (whale_ret_obj_g && WHALE_IS_OBJ(ta) && whale_type_is_num(tb)) {
                    /* A plain object from the OP_object fallback arm: the
                       ordinary define - JS_WhaleStructSetNum would refuse it
                       and every call would deopt. In a reuse loop the first
                       trip defines (transition) and later trips only store
                       the value (shape unchanged, no transition, no prop-array
                       growth): the ru mark tells them apart. */
                    if (whale_plain_slots & (1u << WHALE_OBJ_SLOT(ta))) {
                        int fm = fold_mark();
                        int ridx = -1;
                        int rfc = -1;

                        if (whale_loop_depth > 0) {
                            int hd = whale_loops[whale_loop_depth - 1].head_pos;

                            if (hd >= 0 && hd <= len && whale_loop_reuse[hd].ok)
                                ridx = whale_reuse_ridx(hd);
                        }
                        /* steady-state cache pair for StoreNum (only when
                           reusing: pays one pair off the budget). */
                        if (ridx >= 0) {
                            if (whale_fields >= WHALE_FIELD_CACHE)
                                ridx = -1;
                            else
                                rfc = whale_fields++;
                        }
                        fold_materialize(sp - 1);
                        if (ridx >= 0) {
                            wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ru", ridx); wt(')'); wt('{');
                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum");
                            wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                            wt_int(atom); wt(','); fold_inline(sp - 1); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                            wt('}'); wk(JS_WK_ELSE); wt('{');
                            /* steady state: shape unchanged, so the cached
                               pointer stores with no transition (frees nothing:
                               old value is numeric, GC-owned). */
                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjStoreNum");
                            wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                            wt_int(atom); wt(','); fold_inline(sp - 1); wt(',');
                            wt('&'); wt_arr("fs", rfc); wt(','); wt('&'); wt_arr("fp", rfc);
                            wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                            wt('}');
                        } else {
                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum");
                            wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                            wt_int(atom); wt(','); fold_inline(sp - 1); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        }
                        fold_commit(fm);
                        sp--;   /* the object stays on the stack */
                        break;
                    }
                    /* Still inside the fresh-struct window: the column is a
                       compile-time constant and the store is two plain
                       assignments through the data pointer - no helper, no
                       column scan. This is the hot shape of setField,
                       makeProps and createElement. */
                    if (whale_str_fresh == WHALE_OBJ_SLOT(ta)) {
                        int col = whale_str_col(whale_str_fresh_atoms, whale_str_fresh_n, atom);

                        if (col >= 0) {
                            int fm = fold_mark();

                            fold_materialize(sp - 1);
                            wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                            wt_op(JS_OP_DOT); wt_id("tag"); wt('='); wt_int(8); wt(';');
                            wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                            wt_op(JS_OP_DOT); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt('=');
                            fold_inline(sp - 1); wt(';');
                            fold_commit(fm);
                            sp--;   /* the object stays on the stack */
                            break;
                        }
                    }
                    {
                        int fm = fold_mark();

                        fold_materialize(sp - 1);
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStructSetNum");
                        wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                        wt_int(atom); wt(','); fold_inline(sp - 1); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                        sp--;   /* the object stays on the stack */
                        break;
                    }
                }
                /* The fields of an unboxed struct are frame variables, not
                   properties: the literal's initialisers become direct stores into
                   the layout's columns. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);

                    if (col >= 0 && whale_type_is_num(tb)) {
                        int fm = fold_mark();

                        wt_slot("L", whale_str_L[col]); wt('='); fold_inline(sp - 1); wt(';');
                        wt_arr("sp", col); wt('='); wt_int(1); wt(';');
                        fold_commit(fm);
                        whale_sp_note(col);
                        sp--;   /* the object stays on the stack */
                        break;
                    }
                    /* The layout cannot hold this field, so the object cannot stay a
                       layout:
                         - the atom is not one of the columns (another site widened
                           the layout), or
                         - the value is one the double domain has no room for, such as
                           the string vnode_build stores in `key`.

                       Materialise the columns into a real object once, retype the
                       object slot, and fall through to the engine path below - which
                       already knows how to store an opaque value. Every column is
                       rewritten from `sv`, so a later field of the same object
                       rebuilds whatever an earlier one put there. Abandoning the
                       function here is what kept vnode_build interpreting.

                       This block exists at the call-argument case below as well. It is
                       written out at both instead of factored out because the layout
                       state it reads - whale_str_atoms, whale_str_n,
                       whale_str_obj_slot - is local to this function. */
                    if (whale_str_obj_slot < 0) {
                        if (whale_objs >= WHALE_CALL_BASE)
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
                        whale_str_obj_slot = whale_objs++;
                    }
                    {
                        int fm = fold_mark();
                        int ms = whale_str_obj_slot, m;

                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjNew"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(ms); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        for (m = 0; m < whale_str_n; m++) {
                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum");
                            wt('('); wt_id("ctx"); wt(','); wt_int(ms); wt(',');
                            wt_int(whale_str_atoms[m]); wt(',');
                            wt_slot("L", whale_str_L[m]); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        }
                        fold_commit(fm);
                    }
                    /* Both the tracked type and the local must be updated: the tests
                       below read `ta`, not cur[]. */
                    ta = cur[var_count + sp - 2] = (uint8_t)WHALE_OBJ_T(whale_str_obj_slot);
                }
                /* An object on the stack here is either a plain slot, a call's
                   result, or the one the layout above was just materialised into.
                   WHALE_IS_OBJ and WHALE_IS_VAL cover those, and WHALE_SLOT reads
                   all three because the slot is encoded the same way in each. */
                if (!WHALE_IS_SLOT(ta) && !WHALE_IS_OBJ(ta) && !WHALE_IS_VAL(ta))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (!whale_type_is_num(tb)) {
                    /* { key: "a string" } - the value is opaque, so hand it to the
                       engine rather than abandon the function. This is the last
                       thing standing between vnode_build and compilation. */
                    if (!WHALE_IS_VAL(tb))
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
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

                /* A real (C-struct) object's field is one column of its own storage, so
                   the store is one call that finds the column from the object's offset
                   map. Same code whether the object was built here or handed in by
                   another compiled function - the columns travel with it. */
                if (whale_ret_obj_g && WHALE_IS_OBJ(ta) && whale_type_is_num(tb)) {
                    if (whale_plain_slots & (1u << WHALE_OBJ_SLOT(ta))) {
                        /* plain object: the ordinary property store */
                        int fm = fold_mark();

                        fold_materialize(sp - 1);
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum");
                        wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                        wt_int(atom); wt(','); fold_inline(sp - 1); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                        sp -= 2;
                        break;
                    }
                    if (whale_str_fresh == WHALE_OBJ_SLOT(ta)) {
                        int col = whale_str_col(whale_str_fresh_atoms, whale_str_fresh_n, atom);

                        if (col >= 0) {
                            int fm = fold_mark();

                            fold_materialize(sp - 1);
                            wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                            wt_op(JS_OP_DOT); wt_id("tag"); wt('='); wt_int(8); wt(';');
                            wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                            wt_op(JS_OP_DOT); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt('=');
                            fold_inline(sp - 1); wt(';');
                            fold_commit(fm);
                            sp -= 2;
                            break;
                        }
                    }
                    {
                        int fm = fold_mark();

                        fold_materialize(sp - 1);
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStructSetNum");
                        wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                        wt_int(atom); wt(','); fold_inline(sp - 1); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                        sp -= 2;
                        break;
                    }
                }
                /* A struct field is a frame variable: the store is one
                   assignment - no shape check, no pointer chase, no tag branch.
                   That is the whole point of laying the object out. The flag
                   write stays - unless the access is field-stable (the shape
                   proves presence on every trip, so the runtime flag never
                   changes): then the emission is just the column store, and
                   the note stays so later reads in this segment keep their
                   no-branch form. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);

                    if (col < 0 || !whale_type_is_num(tb))
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    {
                        int fm = fold_mark();
                        int colstable = whale_field_stable(ta, whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1)));

                        wt_slot("L", whale_str_L[col]); wt('='); fold_inline(sp - 1); wt(';');
                        if (!colstable) {
                            wt_arr("sp", col); wt('='); wt_int(1); wt(';');
                        }
                        fold_commit(fm);
                        whale_sp_note(col);
                    }
                    sp -= 2;
                    break;
                }
                if (whale_dbg_env("WHALE_JIT_DUMP"))
                    fprintf(stderr, "[put] pos=%d ta=%d slot=%d tb=%d isnum=%d fields=%d isobj=%d\n",
                            pos, ta, WHALE_IS_SLOT(ta), tb, whale_type_is_num(tb),
                            whale_fields, WHALE_IS_OBJ(ta));
                /* A store this generator cannot lower - an operand that is
                   neither a number nor a parked value, or a shape it has no
                   cache left for - is handed to the engine as one instruction
                   instead of abandoning the function (see whale_step_emit). */
                /* A laid-out struct must not be handed to the engine here: its type is
                   deliberately not part of WHALE_IS_SLOT (a struct has no JSValue in a
                   slot), so `!WHALE_IS_SLOT(ta)` is true for it and this test used to
                   take it - the step fallback ran before the struct branch below ever
                   saw it. Every field store on an unboxed object went through the
                   interpreter that way, which is why `objfields` was unboxed on paper
                   and interpreted in practice. */
                if ((!WHALE_IS_STRUCT(ta) &&
                     (!WHALE_IS_SLOT(ta) || whale_fields >= WHALE_FIELD_CACHE ||
                      (!whale_type_is_num(tb) && !WHALE_IS_VAL(tb)))) &&
                    whale_step_emit(OP_put_field, pos, &sp, cur, var_count,
                                    stack_size))
                    break;
                if (!WHALE_IS_SLOT(ta))
                    { whale_out_line = __LINE__; goto out; }
                if (whale_fields >= WHALE_FIELD_CACHE)
                    { whale_out_line = __LINE__; goto out; }
                if (!whale_type_is_num(tb) && !WHALE_IS_VAL(tb))
                    { whale_out_line = __LINE__; goto out; }
                {
                    int fc = whale_fields++;
                    int fm;
                    /* Poly-2 second pair: only when the warm-up saw exactly two
                       shapes here (each pair holds one resolved pointer). Costs
                       one more pair off the 24-pair budget; -1 keeps the
                       single-pair path. */
                    int fc2 = -1;

                    if (whale_prof_two_shapes((unsigned)(pos + 1)) &&
                        whale_fields < WHALE_FIELD_CACHE) {
                        fc2 = whale_fields++;
                        whale_prof_two_off = (unsigned)(pos + 1);
                    }

                    /* `stable` means "the shape cannot change for the rest of
                       this function", which was proved about a local literal.
                       An object that came from a call is not one, so it always
                       keeps its shape check.
                       Object locals always travel as VAL slots (put_loc unifies
                       to VAL_T, never OBJ), so WHALE_IS_OBJ alone is dead - it
                       never fires for a local. A VAL slot is equally stable
                       when no loop body can swap its identity: no calls (escape
                       + reshape from outside) and no fresh object or store
                       into an object local inside a loop (whale_loop_shape_
                       dirty). Field-value stores (put_field) never touch the
                       identity, so plain `o.x += v` traffic stays stable.
                       The call gate is bytecode calls only (has_bcall): helper
                       calls from lowering (fmod/ObjShape/fieldPtr) cannot
                       reshape user objects, while gating on has_call would
                       close stability for exactly the object loops it helps -
                       every one of them lowers field access to helpers. */
                    /* In a reuse loop the object's shape is fixed after the
                       first trip (same slot reused, fields only overwritten):
                       resolve once, then store straight through the cached
                       pointer. The ru mark (set by the loop's OP_object when
                       it built) tells them apart. Outside reuse the resolve
                       runs every trip as before. */
                    {
                        int ridx = -1;

                        if (whale_loop_depth > 0) {
                            int hd = whale_loops[whale_loop_depth - 1].head_pos;

                            if (hd >= 0 && hd <= len && whale_loop_reuse[hd].ok)
                                ridx = whale_reuse_ridx(hd);
                        }
                        if (ridx >= 0)
                            fold_obj_field_resolve(WHALE_SLOT(ta), atom, fc, fc2, 1);
                        else
                            fold_obj_field_resolve(WHALE_SLOT(ta), atom, fc, fc2,
                                                   whale_field_stable(ta, whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1)) || whale_loop_stable_here()));
                    }
                    fm = fold_mark();
                    if (whale_type_is_num(tb)) {
                        /* the value is a double here, and the old one was a number
                           too (anything else deopted above). In a reuse loop
                           the tag is already 8 from the first trip, so only
                           the value store runs on later trips - the ru mark
                           guards the tag write the same way. */
                        {
                            int hd2 = (whale_loop_depth > 0)
                                ? whale_loops[whale_loop_depth - 1].head_pos : -1;
                            int ridx2 = (hd2 >= 0 && hd2 <= len && whale_loop_reuse[hd2].ok)
                                ? whale_reuse_ridx(hd2) : -1;

                            if (ridx2 >= 0) {
                                wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ru", ridx2); wt(')'); wt('{');
                                wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt('='); wt_int(8); wt(';');
                                wt('}');
                            } else {
                                wt_arr("fp", fc); wt_op(JS_OP_ARROW); wt_id("tag"); wt('='); wt_int(8); wt(';');
                            }
                        }
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

                /* A real (C-struct) receiver: the receiver keeps its slot and the column
                   is pushed above it. The helper finds the column from the object's
                   offset map, so this is the same code whether the object was built here
                   or handed in by another compiled function. */
                if (whale_ret_obj_g && WHALE_IS_OBJ(ta)) {
                    nm = whale_vserial++;
                    {
                        int fm = fold_mark();

                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStructFieldNum");
                        wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                        wt_int(atom); wt(','); wt('&'); wt_arr("v", nm); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;
                    whale_vexpr[sp].inv = 0;   /* a memory read */
                    cur[var_count + sp] = WHALE_T_NUM;
                    sp++;
                    break;
                }
                /* The receiver stays on the stack under the value - that is what
                   `get_field2` is for - so a struct receiver keeps its slot type
                   and the field value is pushed above it. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);

                    if (col < 0 || whale_vserial >= stack_size + WHALE_VSER_MAX)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    nm = whale_vserial++;
                    {
                        int fm = fold_mark();

                        /* Skip the check when this segment already proved the
                           column present (whale_sp_known/note) - objfields'
                           read-after-write of one column then compiles to just
                           the copy, with no branch at all. A field-stable
                           access skips it too: the shape proves the column
                           present on every trip (the loop pre-fill ran once
                           outside), so neither the check nor the flag write
                           is needed - but the note stays, so later reads in
                           this segment keep their no-branch form. */
                        if (!whale_sp_known(col) && !whale_field_stable(ta, whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1)))) {
                            wk(JS_WK_IF); wt('('); wt('!'); wt_arr("sp", col); wt(')'); wt('{');
                            wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                            whale_sp_note(col);
                        } else {
                            whale_sp_note(col);
                        }
                        wt_arr("v", nm); wt('='); wt_slot("L", whale_str_L[col]); wt(';');
                        fold_commit(fm);
                        whale_vname[sp] = nm;
                        whale_vmat[sp] = 1;
                        whale_vexpr[sp].inv = 0;   /* a memory read */
                    }
                    cur[var_count + sp] = WHALE_T_NUM;
                    sp++;
                    break;
                }
                if (!WHALE_IS_SLOT(ta))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (whale_fields >= WHALE_FIELD_CACHE)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                fc = whale_fields++;
                nm = whale_vserial++;
                /* VAL slots ride the same stability as OBJ (see the resolve
                   site above: locals unify to VAL, so IS_OBJ alone is dead).
                   Poly-2 second pair when the site saw two shapes (each pair
                   holds one resolved pointer); -1 keeps the single pair. */
                {
                    int fc2 = -1;

                    if (whale_prof_two_shapes((unsigned)(pos + 1)) &&
                        whale_fields < WHALE_FIELD_CACHE) {
                        fc2 = whale_fields++;
                        whale_prof_two_off = (unsigned)(pos + 1);
                    }
                    fold_obj_field_load(WHALE_SLOT(ta), atom, fc, fc2, nm,
                                        whale_field_stable(ta, whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1)) || whale_loop_stable_here()));
                }
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

                /* A real (C-struct) object's field is one column of its own storage: one
                   call that finds the column from the object's offset map and reads it.
                   A reserved column reads undefined; the helper refuses anything that is
                   not a plain number, and the ordinary path below takes over then. The
                   same code whether the object was built here or handed in by another
                   compiled function. */
                if (whale_ret_obj_g && WHALE_IS_OBJ(ta)) {
                    /* Fresh window: the read is the column through the data
                       pointer - `t.v` in `t.u = t.v * 3` right after the
                       literal was built. Tag-checked inline, no helper. */
                    if (whale_str_fresh == WHALE_OBJ_SLOT(ta)) {
                        int col = whale_str_col(whale_str_fresh_atoms, whale_str_fresh_n, atom);
                        int nm;

                        if (col >= 0) {
                            nm = whale_vserial++;
                            {
                                int fm = fold_mark();

                                wt_arr("v", nm); wt('=');
                                wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                                wt_op(JS_OP_DOT); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt('?');
                                wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                                wt_op(JS_OP_DOT); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64");
                                wt(':'); wt('('); wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                                wt_op(JS_OP_DOT); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt('?');
                                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt_id("_sdp"); wt('['); wt_int(col); wt(']');
                                wt_op(JS_OP_DOT); wt_id("u"); wt_op(JS_OP_DOT); wt_id("int32");
                                wt(':'); wt_id("JS_WhaleObjValueNum"); wt('('); wt_id("ctx"); wt(',');
                                wt('&'); wt_id("_sdp"); wt('['); wt_int(col); wt(']'); wt(','); wt_id("deopt"); wt(')');
                                wt(';');
                                fold_commit(fm);
                            }
                            whale_vname[sp - 1] = nm;
                            whale_vmat[sp - 1] = 1;
                            whale_vexpr[sp - 1].inv = 0;   /* a memory read */
                            cur[var_count + sp - 1] = WHALE_T_NUM;
                            break;
                        }
                    }
                    nm = whale_vserial++;
                    {
                        int fm = fold_mark();

                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleStructFieldNum");
                        wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_OBJ_SLOT(ta)); wt(',');
                        wt_int(atom); wt(','); wt('&'); wt_arr("v", nm); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                    }
                    whale_vname[sp - 1] = nm;
                    whale_vmat[sp - 1] = 1;
                    whale_vexpr[sp - 1].inv = 0;   /* a memory read */
                    cur[var_count + sp - 1] = WHALE_T_NUM;
                    break;
                }
                /* A struct field is a frame variable: one load, with a presence
                   flag in place of a tag branch. The flag matters because a field
                   the layout reserves but this object never assigned must not read
                   as a number. */
                if (WHALE_IS_STRUCT(ta)) {
                    int col = whale_str_col(whale_str_atoms, whale_str_n, atom);
                    int fm;

                    if (col < 0)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    /* The presence flag is a statement, so it is emitted on its own,
                       closed off with a commit. It also has to come first: it reads
                       sp[col], which nothing in the expression below touches.
                       Skip it when this segment already proved the column (same
                       rule as the get_field2 arm above), or when the access is
                       field-stable (shape-proved presence on every trip; the
                       note stays so later reads keep their form). */
                    if (!whale_sp_known(col) && !whale_field_stable(ta, whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1)))) {
                        fm = fold_mark();
                        wk(JS_WK_IF); wt('('); wt('!'); wt_arr("sp", col); wt(')'); wt('{');
                        wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        fold_commit(fm);
                        whale_sp_note(col);
                    } else {
                        whale_sp_note(col);
                    }
                    /* The column is the value. It is a register scalar already, so
                       reading it where it is used costs one operand - copying it into
                       _ws.v[] first put a store and a reload on every field access,
                       and it is the round trip, not the arithmetic, that a chain of
                       field reads pays for. Same rule as a deferred local: the column
                       stays in the expression until its length ceiling is hit or the
                       block ends, and fold_materialize() gives it a real slot then. */
                    fm = fold_mark();
                    wt_slot("L", whale_str_L[col]);
                    /* A memory read: the column can change between iterations
                       (a store through another slot), so never invariant. */
                    fold_set(sp - 1, fm, 0);
                    cur[var_count + sp - 1] = WHALE_T_NUM;
                    break;
                }
                /* Refusals below (the receiver is not an object slot, or the
                   cache is full) become one instruction handed to the engine - but
                   not a laid-out struct, whose type is deliberately outside
                   WHALE_IS_SLOT. Same trap as the store: the step fallback came
                   first and took every read on an unboxed object. */
                if (!WHALE_IS_STRUCT(ta) &&
                    (!WHALE_IS_SLOT(ta) || whale_fields >= WHALE_FIELD_CACHE) &&
                    whale_step_emit(OP_get_field, pos, &sp, cur, var_count,
                                    stack_size))
                    break;
                if (!WHALE_IS_SLOT(ta))
                    { whale_out_line = __LINE__; goto out; }
                if (whale_fields >= WHALE_FIELD_CACHE)
                    { whale_out_line = __LINE__; goto out; }
                fc = whale_fields++;
                nm = whale_vserial++;
                /* VAL slots ride the same stability as OBJ (see the resolve
                   site above: locals unify to VAL, so IS_OBJ alone is dead).
                   Poly-2 second pair when the site saw two shapes (each pair
                   holds one resolved pointer); -1 keeps the single pair. */
                {
                    int fc2 = -1;

                    if (whale_prof_two_shapes((unsigned)(pos + 1)) &&
                        whale_fields < WHALE_FIELD_CACHE) {
                        fc2 = whale_fields++;
                        whale_prof_two_off = (unsigned)(pos + 1);
                    }
                    fold_obj_field_load(WHALE_SLOT(ta), atom, fc, fc2, nm,
                                        whale_field_stable(ta, whale_shape_stable || whale_prof_one_shape((unsigned)(pos + 1)) || whale_loop_stable_here()));
                }
                whale_vname[sp - 1] = nm;
                whale_vmat[sp - 1] = 1;
                whale_vexpr[sp - 1].inv = 0;   /* a memory read */
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }           /* only the empty literal `[]` */
                if (whale_objs >= WHALE_CALL_BASE)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                    { whale_out_line = __LINE__; goto out; }
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
                    whale_vexpr[sp - 1].inv = 0;   /* a memory read */
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
                    if (!whale_type_is_num(tb))
                        { whale_out_line = __LINE__; goto out; }
                    if (!whale_type_is_num(tc)) {
                        /* `out[i] = t` with t an object (or any parked value):
                           one helper store out of the slot. This is the shape
                           objshape's escaped() phase is - without it every
                           function that keeps an object in an array is
                           refused. A STRUCT value (frame columns, no JSValue
                           in any slot) cannot travel this way: materialise it
                           into the shared object slot first (same pattern as
                           the call-argument case above), then store that. */
                        int mtc = tc;

                        if (WHALE_IS_STRUCT(tc)) {
                            if (whale_str_obj_slot < 0) {
                                if (whale_objs >= WHALE_CALL_BASE)
                                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                                whale_str_obj_slot = whale_objs++;
                            }
                            {
                                int mfm = fold_mark();
                                int mms = whale_str_obj_slot, mm;
                                /* In-loop materialisation reuse: when this store
                                   is the recognised park of a reuse loop, the
                                   heap object is built once (first trip) and
                                   reused - later trips only refresh the columns
                                   (plain L stores, already emitted) and repark
                                   the same reference. The ru mark tells them
                                   apart; the array store below stays (length
                                   semantics + rotating index). */
                                int ridx = -1;

                                if (whale_loop_depth > 0) {
                                    int hd = whale_loops[whale_loop_depth - 1].head_pos;

                                    if (hd >= 0 && hd <= len && whale_loop_reuse[hd].ok &&
                                        whale_loop_reuse[hd].arrpos == pos)
                                        ridx = whale_reuse_ridx(hd);
                                }
                                if (ridx >= 0)
                                    { wk(JS_WK_IF); wt('('); wt('!'); wt_arr("ru", ridx); wt(')'); wt('{'); }
                                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjNew"); wt('(');
                                wt_id("ctx"); wt(','); wt_int(mms); wt(')'); wt(')');
                                wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                                wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                                for (mm = 0; mm < whale_str_n; mm++) {
                                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum");
                                    wt('('); wt_id("ctx"); wt(','); wt_int(mms); wt(',');
                                    wt_int(whale_str_atoms[mm]); wt(',');
                                    wt_slot("L", whale_str_L[mm]); wt(')'); wt(')');
                                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                                }
                                if (ridx >= 0) {
                                    wt_arr("ru", ridx); wt('='); wt_int(1); wt(';');
                                    wt('}');
                                }
                                fold_commit(mfm);
                            }
                            mtc = WHALE_OBJ_T(whale_str_obj_slot);
                        }
                        if (!WHALE_IS_SLOT(mtc))
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
                        os = WHALE_OBJ_SLOT(ta);
                        tk = whale_vserial++;      /* the index, evaluated once */
                        {
                            int fm = fold_mark();

                            /* a store can move the backing store (the array
                               grows), so the cached pointer must not be
                               trusted afterwards */
                            wt_arr("ai", os); wt('='); wt_int(0); wt(';');
                            wt_arr("v", tk); wt('='); fold_inline(sp - 2); wt(';');
                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleArrSetVal");
                            wt('('); wt_id("ctx"); wt(','); wt_int(os); wt(',');
                            wt_arr("v", tk); wt(','); wt_int(WHALE_SLOT(mtc)); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                            fold_commit(fm);
                        }
                        sp -= 3;
                        break;
                    }
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
                    { whale_out_line = __LINE__; goto out; }
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                    int li = 0;

                    /* Int-region mirror read: inside the loop a mirrored
                       local runs on I<idx> (a C int, zero conversion). A
                       pending expression, if any, is itself an int
                       expression (mirror stores wrap `(int)`), so inlining
                       it keeps int meaning. Outside the loop the exit edge
                       has boxed the mirror back, so L<idx> is current.
                       The value is int32 by construction: type I32, which
                       makes every bitwise consumer take the short form. */
                    if (whale_int_ok && idx >= 0 && idx < WHALE_JIT_MAX_SLOTS &&
                        whale_int_mirror[idx] && whale_loop_depth > 0 &&
                        whale_lmem) {
                        if (whale_lmem[idx].live)
                            fold_inline_expr(&whale_lmem[idx].expr);
                        else
                            wt_slot("I", idx);
                        fold_set(sp, fm, 0);
                        /* IBIT, not I32: a mirror read is a bare int (the
                           variable itself, or an `(int)`-wrapped pending
                           expression), so bitwise consumers skip every cast. */
                        cur[var_count + sp] = WHALE_T_IBIT;
                        sp++;
                        break;
                    }
                    local_inline(idx);
                    /* Invariance of a local read: a constant local inlines as
                       its literal (invariant); a still-pending expression keeps
                       whatever it was when the local was stored; an already
                       materialised local is a variable read, invariant only if
                       the loop body never writes it. Decided before fold_set:
                       a long pending expression materialises inside, and the
                       hoist decision needs the value already in place. */
                    if (idx >= 0 && idx < WHALE_JIT_MAX_SLOTS) {
                        if (whale_lo_c[idx])
                            li = 1;
                        else if (whale_lmem[idx].live)
                            li = whale_lmem[idx].expr.inv;
                        else if (whale_local_invariant(idx))
                            li = 1;
                    }
                    fold_set(sp, fm, li);
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (whale_dbg_env("WHALE_MEM_TRACE"))
                    fprintf(stderr, "[put_loc] L%d t=%d is_struct=%d is_slot=%d\n",
                            idx, t, WHALE_IS_STRUCT(t), WHALE_IS_SLOT(t));
                /* An object that arrived from elsewhere - a call's result, or an
                   inlined body's return - gets a layout too, provided nothing in
                   this function can change its property table behind our back. The
                   conservative form of that: the function contains no call at all.
                   Objects that do not qualify keep the shape-cache path, which is
                   the "fall back to the map" this is allowed to do.

                   Which function created the object is deliberately not part of the
                   test: what was observed is what its fields are, and the
                   observation is keyed by the access sites.

                   The safety condition is "nothing after this point can change the
                   object's property table", and the only thing that can is a call -
                   a callee may add, delete or reorder properties. So the test is
                   whether any call remains after this instruction, not whether the
                   function has one at all: `run()` calls `make()` once at the top
                   and then loops without calling anything, which is exactly the
                   shape worth laying out. */
                if (whale_dbg_env("WHALE_JIT_DUMP"))
                    fprintf(stderr, "[fill] pos=%d t=%d strn=%d calln=%d sites=%d mono=%d\n",
                            pos, t, whale_str_n, whale_call_n, whale_field_sites_n,
                            whale_sites_freq(whale_field_sites, whale_field_sites_n));
                if (!whale_dbg_env("WHALE_NO_UNBOX") && whale_str_n == 0 &&
                    (whale_call_n == 0 || pos > whale_call_pos[whale_call_n - 1]) &&
                    (WHALE_IS_OBJ(t) || WHALE_IS_VAL(t)) && !whale_generic_mode &&
                    whale_sites_freq(whale_field_sites, whale_field_sites_n)) {
                    int fn = whale_layout_from_sites(whale_field_sites,
                                                     whale_field_sites_n,
                                                     whale_str_atoms,
                                                     WHALE_ATOM_MAX,
                                                     info->prefer_gcc ? 0 : 40);
                    int slot = WHALE_SLOT(t);

                    if (fn > 0) {
                        int k, pfm = fold_mark();

                        for (k = 0; k < fn; k++) {
                            /* Read each field once, into its column. The object
                               itself is left alone - these are what the accesses
                               below use. A fill proves every column it reads,
                               so note them all for the presence-flag dedup. */
                            wt_slot("L", whale_str_L[k]); wt('=');
                            wt_id("JS_WhaleObjFieldNum");
                            wt('('); wt_id("ctx"); wt(','); wt_int(slot); wt(',');
                            wt_int(whale_str_atoms[k]); wt(',');
                            wt('&'); wt_arr("fs", WHALE_FIELD_CACHE - 1); wt(',');
                            wt('&'); wt_arr("fp", WHALE_FIELD_CACHE - 1); wt(',');
                            wt_id("deopt"); wt(')'); wt(';');
                            wt_arr("sp", k); wt('='); wt_int(1); wt(';');
                        }
                        fold_commit(pfm);
                        for (k = 0; k < fn; k++)
                            whale_sp_note(k);
                        whale_str_n = fn;
                        whale_str_slot = slot;
                        local_forget(idx);
                        cur[idx] = WHALE_STRUCT_T(slot);
                        whale_lo_c[idx] = 0;
                        sp--;
                        break;
                    }
                }
                if (WHALE_IS_STRUCT(t)) {
                    /* A struct local carries the type and nothing else: the layout
                       columns (_ws.sv / _ws.sp) are the storage, so there is no C
                       variable and no object slot.

                       This has to be tested *before* WHALE_IS_SLOT, which
                       deliberately does not include structs. Placed inside that
                       branch it is unreachable - and then the deferred-store path
                       emits `L0 = ( )`, because a struct has no expression to
                       inline. */
                    if (loc_slot[idx] < 0)
                        loc_slot[idx] = WHALE_SLOT(t);
                    local_forget(idx);
                    cur[idx] = WHALE_STRUCT_T(loc_slot[idx]);
                    whale_lo_c[idx] = 0;
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
                        whale_lo_c[idx] = 0;
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
                        whale_lo_c[idx] = 0;
                        sp--;
                        break;
                    }
                    {
                        if (loc_slot[idx] < 0) {
                            if (whale_objs >= WHALE_CALL_BASE)
                                { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                    whale_lo_c[idx] = 0;
                    /* No fver bump: assigning an object only swaps which heap
                       object the local names. No property table changed - no
                       field was added, removed, or reordered - so every cached
                       value pointer stays valid. The bump used to sit here
                       unconditionally, and a loop-carried object local (the
                       Redux `st`, reassigned from a call every iteration) then
                       re-resolved every field on every trip for nothing. A
                       store *through* the object (put_field/define_field on a
                       heap object, or a call that may do either) still bumps -
                       see those sites. */
                    sp--;
                    break;
                }
                /* Defer the store: the value stays an expression until it is
                   read, or until the basic block ends. */
                local_forget(idx);
                fm = fold_mark();
                /* Int-region mirror store: the pending expression must read
                   as an int inside the loop (mirror reads inline it or load
                   I<idx>), so wrap the value in `(int)` here. The value is
                   int32 by recognition (int bitwise producer), so the cast
                   is exact - and the materialise path below stores the same
                   wrapped expression into I<idx>. Non-mirrors are untouched. */
                if (whale_int_ok && idx >= 0 && idx < WHALE_JIT_MAX_SLOTS &&
                    whale_int_mirror[idx] && whale_loop_depth > 0) {
                    wt('('); wk(JS_WK_INT); wt(')');
                    wt('('); fold_inline(sp - 1); wt(')');
                    local_set(idx, fm, whale_vexpr[sp - 1].inv);
                    /* IBIT: the pending expression is `(int)(...)`, a bare
                       int - mirror reads inline it with zero conversion. */
                    cur[idx] = WHALE_T_IBIT;
                    whale_lo_c[idx] = 0;
                    sp--;
                    break;
                }
                fold_inline(sp - 1);
                local_set(idx, fm, whale_vexpr[sp - 1].inv);
                cur[idx] = (uint8_t)t;
                whale_lo_c[idx] = 0;
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (WHALE_IS_STRUCT(t)) {
                    /* See put_loc: a struct local carries the type and nothing
                       else - the layout columns are the storage. The value stays
                       on the stack, which is what set_loc is for. */
                    if (loc_slot[idx] < 0)
                        loc_slot[idx] = WHALE_SLOT(t);
                    local_forget(idx);
                    cur[idx] = WHALE_STRUCT_T(loc_slot[idx]);
                    whale_lo_c[idx] = 0;
                    break;
                }
                if (WHALE_IS_ARR(t)) {
                    /* An array keeps its own type - see the note in put_loc. */
                    local_forget(idx);
                    fm = fold_mark();
                    wt_arr("oi", WHALE_SLOT(t)); wt('='); wt_int(0); wt(';');
                    fold_commit(fm);
                    cur[idx] = (uint8_t)t;
                    whale_lo_c[idx] = 0;
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
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                    whale_lo_c[idx] = 0;
                    /* No fver bump here either - same reason as put_loc above:
                       only the identity in the local changed, no property
                       table. */
                    break;
                }
                local_forget(idx);
                fm = fold_mark();
                fold_inline(sp - 1);
                local_set(idx, fm, whale_vexpr[sp - 1].inv);
                cur[idx] = (uint8_t)t;
                whale_lo_c[idx] = 0;
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (WHALE_IS_VAL(cur[idx]) && WHALE_IS_VAL(t)) {
                    /* `s += "x"` - compound assignment on opaque values. The
                        general binop runs on the parked slots and the result
                        lands back in the local's own slot, so the slot
                        identity - and with it the jump-target merge - is
                        unchanged. This is what kept stringcat from compiling. */
                    fm = fold_mark();
                    wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleBinopVal");
                    wt('('); wt_id("ctx"); wt(','); wt_int(OP_add); wt(',');
                    wt_int(WHALE_VAL_SLOT(cur[idx])); wt(',');
                    wt_int(WHALE_VAL_SLOT(t)); wt(',');
                    wt_int(WHALE_VAL_SLOT(cur[idx])); wt(')'); wt(')');
                    wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    sp--;
                    break;
                }
                if (!whale_type_is_num(cur[idx]) || !whale_type_is_num(t))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                /* Folds to one expression as well. The left operand is the
                   current value, which may itself still be pending, so that is
                   written out first - before the new range is marked. */
                local_materialize(idx);
                fm = fold_mark();
                wt_slot("L", idx); wt('+'); fold_inline(sp - 1);
                /* The expression names L<idx>, a loop-carried variable: never
                   invariant, whatever the right operand is. */
                local_set(idx, fm, 0);
                cur[idx] = WHALE_T_NUM;
                whale_lo_c[idx] = 0;
                sp--;
            }
            break;
        case OP_get_loc0_loc1:
            if (var_count < 2 || cur[0] == WHALE_T_UNKNOWN || cur[1] == WHALE_T_UNKNOWN)
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            {
                int fm = fold_mark();
                int li0 = 0, li1 = 0;

                local_inline(0);
                if (whale_lo_c[0])
                    li0 = 1;
                else if (whale_lmem[0].live)
                    li0 = whale_lmem[0].expr.inv;
                else if (whale_local_invariant(0))
                    li0 = 1;
                fold_set(sp, fm, li0);
                fm = fold_mark();
                local_inline(1);
                if (whale_lo_c[1])
                    li1 = 1;
                else if (whale_lmem[1].live)
                    li1 = whale_lmem[1].expr.inv;
                else if (whale_local_invariant(1))
                    li1 = 1;
                fold_set(sp + 1, fm, li1);
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (arg_obj_slot[idx] >= 0) {
                    /* This argument travels parked in its slot: as an array
                       when the body indexes it, otherwise as an opaque value. */
                    cur[var_count + sp] = args_arr[idx]
                        ? (uint8_t)WHALE_ARR_T(arg_obj_slot[idx])
                        : (uint8_t)WHALE_VAL_T(arg_obj_slot[idx]);
                    whale_vmat[sp] = 1;
                    sp++;
                    break;
                }
                fm = fold_mark();
                wt_slot("A", idx);
                fold_set(sp, fm, 1);   /* arguments never change */
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
                    int fm = 0;

                    /* The result now depends on something other than the
                        arguments - the captured state - so the engine must not
                        memoise this function. Without this a counter closure
                        returns the first answer for ever. */
                    whale_gen_pure = 0;

                    /* Through the storage cell (fetched once in the prologue):
                       a tag-checked load, no helper. `n = n + 1` in a compiled
                       closure loop costs a load, an add and a store. */
                    fm = fold_mark();
                    wk(JS_WK_IF); wt('('); wt_slot("_p", idx);
                    wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8); wt(')'); wt('{');
                    wt_arr("v", nm); wt('='); wt_slot("_p", idx);
                    wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT); wt_id("float64"); wt(';');
                    wt('}'); wk(JS_WK_ELSE); wk(JS_WK_IF); wt('('); wt_slot("_p", idx);
                    wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(0); wt(')'); wt('{');
                    wt_arr("v", nm); wt('='); wt('('); wk(JS_WK_DOUBLE); wt(')');
                    wt_slot("_p", idx); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT);
                    wt_id("int32"); wt(';');
                    wt('}'); wk(JS_WK_ELSE); wt('{');
                    wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;
                    whale_vexpr[sp].inv = 0;   /* captured state: not invariant */
                    cur[var_count + sp] = WHALE_T_NUM;
                    sp++;
                    break;
                }                if (!whale_assume_self) {
                    /* The other reading of the same bytecode: the slot holds an
                       enclosing closure's value. It is parked once in the
                       prologue (see the whale_vr_slot section) - the captured
                       value cannot change during this call, the same fact the
                       entry's self-check relies on - so every read here just
                       pushes the parked reference.

                       And if the parked value is a bytecode function - the
                       React shape, a component calling a helper - the call can
                       be expanded inline behind an identity guard: WHALE_T_VRFN
                       tells the call site to emit the guard plus the inlined
                       body. Resolution happens once, here, at compile time. */
                    int slot = whale_vr_slot[idx];

                    if (slot < 0)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    whale_gen_pure = 0;   /* depends on captured state */
                    if (whale_vr_fn[idx]) {
                        /* resolved on an earlier read: the call site expands
                           this inline behind the identity guard. Note cur[idx]
                           is NOT set - the var_ref index and the local index
                           are different spaces, and writing here polluted a
                           local's type (step's `i` came out as a function). */
                        cur[var_count + sp] = WHALE_VRFN_T(idx);
                    } else {
                        void *fn = NULL;

                        if (whale_vr_inline_on() &&
                            JS_WhaleVarRefFuncInfo(ctx, idx, &fn,
                                                   &whale_vr_info[idx]) &&
                            whale_vr_info[idx].is_normal &&
                            whale_vr_info[idx].var_count == 0 &&
                            whale_vr_info[idx].arg_count <= 4) {
                            int gi = JS_WhaleVrExpectAdd(fn);

                            if (gi >= 0) {
                                whale_vr_fn[idx] = fn;
                                /* the guard's argument. Leaving this at -1 made
                                   JS_WhaleVarRefIsFn answer 0 unconditionally,
                                   so the compiled body deopted on every entry
                                   and retired after eight tries - compiled, and
                                   exactly as slow as the interpreter. */
                                whale_vr_expect_slot[idx] = gi;
                                cur[var_count + sp] = WHALE_VRFN_T(idx);
                                sp++;
                                break;
                            }
                        }
                        cur[var_count + sp] = WHALE_VAL_T(slot);
                    }
                    sp++;
                    break;
                }
                (void)idx;
                cur[var_count + sp] = WHALE_T_SELF;
                /* SELF has no value expression: mark the slot materialised so
                   a later use can never inline whatever was recorded there
                   before (the slot is only ever consumed as a call target) -
                   and clear whatever invariance that record had. */
                whale_vmat[sp] = 1;
                whale_vexpr[sp].inv = 0;
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                whale_gen_pure = 0;   /* writes captured state */
                /* Through the storage cell: the old value is known to be a
                   number (anything else deopted on the read that produced it,
                   and the initial undefined deopts the first write too), so
                   overwriting it needs no JS_FreeValue. One tag-checked
                   store, no helper call. */
                fm = fold_mark();
                wk(JS_WK_IF); wt('('); wt_slot("_p", idx);
                wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(8);
                wt_op(JS_OP_AND); wt_slot("_p", idx);
                wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_NE); wt_int(0); wt(')'); wt('{');
                wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                wt_slot("_p", idx); wt_op(JS_OP_ARROW); wt_id("u"); wt_op(JS_OP_DOT);
                wt_id("float64"); wt('='); fold_inline(sp - 1); wt(';');
                wt_slot("_p", idx); wt_op(JS_OP_ARROW); wt_id("tag"); wt('='); wt_int(8); wt(';');                fold_commit(fm);
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
                if (whale_dbg_env("WHALE_JIT_DUMP"))
                    fprintf(stderr, "[getvar] pos=%d sz=%d atom=%d feeds_call=%d\n",
                            pos, sz, atom, whale_feeds_call_as_callee(pc, len, pos, sz));
                if (whale_feeds_call_as_callee(pc, len, pos, sz)) {
                    /* The value is about to be called, so it has to travel as
                       the JSValue it is. A slot is the only way the generated
                       code can hold something that is not a number. A wrong
                       guess here costs the optimisation, never correctness:
                       nothing but a call accepts WHALE_T_VAL. */
                    int slot = whale_objs++;
                    int fm2;

                    if (slot >= WHALE_CALL_BASE)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    nm = whale_vserial++;
                    wk(JS_WK_IF); wt('('); wt('!');
                    wt_id("JS_WhaleGetGlobalObj"); wt('('); wt_id("ctx"); wt(','); wt_int(atom); wt(','); wt_int(slot); wt(')');
                    wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';'); wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                    fold_commit(fm);
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;   /* marks the fold slot dead; unused for objects */
                    whale_vexpr[sp].inv = 0;
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
                       variable now rather than a folded expression - and a
                       memory read, so never invariant */
                    whale_vname[sp] = nm;
                    whale_vmat[sp] = 1;
                    whale_vexpr[sp].inv = 0;
                }
                cur[var_count + sp] = WHALE_T_NUM;
                sp++;
            }
            break;

        /* method call: [... receiver, method, arg1..argN ]. `arr.push(x)` and
           `parts.join(",")` - the whole Array.prototype surface a front-end
           loop touches. The receiver and the method park in their own slots
           and the call runs as one engine entry with the receiver as `this`;
           the result follows the same drop-or-park rule as a plain call.
           tail_call_method is the same instruction - the bytecode drops the
           frame right after, which is exactly what this emit does anyway. */
        case OP_call_method:
        case OP_tail_call_method:
            {
                int argc = (int)whale_get_u16(pc + pos + 1);
                int sp_new = sp - 2 - argc;
                int k, fm, ret_slot, nxt = pos + sz;

                if (sp_new < 0 || argc + 2 > WHALE_CALL_SLOTS)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                /* every operand has to be parkable */
                for (k = 0; k < argc + 2; k++) {
                    int tk = cur[var_count + sp_new + k];

                    if (!whale_type_is_num(tk) && !WHALE_IS_SLOT(tk))
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                }
                if (nxt < len && (int)pc[nxt] == OP_drop) {
                    ret_slot = -1;
                } else {
                    if (whale_objs >= WHALE_CALL_BASE)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    ret_slot = whale_objs++;
                }
                fm = fold_mark();
                /* receiver at CALL_BASE+argc, method right after it, then args */
                for (k = 0; k < argc + 2; k++) {
                    int tk = cur[var_count + sp_new + k];

                    wk(JS_WK_IF); wt('('); wt('!');
                    if (whale_type_is_num(tk)) {
                        wt_id("JS_WhaleObjSlotNum"); wt('('); wt_id("ctx"); wt(',');
                        wt_int(WHALE_CALL_BASE + k); wt(',');
                        fold_inline(sp_new + k); wt(')');
                    } else {
                        wt_id("JS_WhaleObjSlotCopy"); wt('('); wt_id("ctx"); wt(',');
                        wt_int(WHALE_SLOT(tk)); wt(',');
                        wt_int(WHALE_CALL_BASE + k); wt(')');
                    }
                    wt(')'); wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                }
                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleCallMethodValue");
                wt('('); wt_id("ctx"); wt(','); wt_int(WHALE_CALL_BASE + argc + 1); wt(',');
                wt_int(WHALE_CALL_BASE + argc); wt(','); wt_int(argc); wt(',');
                wt_int(WHALE_CALL_BASE); wt(','); wt_int(ret_slot); wt(')'); wt(')');
                wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(2); wt(';');
                wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                fold_commit(fm);
                cur[var_count + sp_new] = (ret_slot < 0)
                    ? (uint8_t)WHALE_T_UNKNOWN : WHALE_VAL_T(ret_slot);
                sp = sp_new + 1;
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                /* A closure-slot call target resolved at compile time: expand
                    the callee inline behind an identity guard - the slot must
                    still hold the exact function this code was compiled
                    against. One pointer compare per call replaces the whole
                    engine-call chain. */
                if (WHALE_IS_VRFN(cur[var_count + func_slot]) && !whale_inline_nested) {
                    int vidx = WHALE_VRFN_IDX(cur[var_count + func_slot]);
                    int rtype, saved_nested = whale_inline_nested;
                    int guard_ok = 1;

                    for (k = 0; k < argc; k++) {
                        int tk = cur[var_count + sp - argc + k];

                        if (!whale_type_is_num(tk) && !WHALE_IS_SLOT(tk))
                            guard_ok = 0;
                    }
                    if (guard_ok) {
                        /* Same-shape factory first: `st = setField(st, v)` is not
                           a body to expand but a store to emit - one field-cache
                           write of the changed column into the source object,
                           no StructNew, no call, no copy. The probe decides;
                           a 0 falls through to the normal body-inline path. */
                        const uint8_t *cpc = whale_vr_info[vidx].byte_code;
                        int clen = whale_vr_info[vidx].byte_code_len, cpos;
                        int csz;

                        fm = fold_mark();   /* the stray argument tokens start here */
                        if (whale_fields < WHALE_FIELD_CACHE &&
                            whale_fuse_probe(&whale_vr_info[vidx], cpc, clen,
                                             cur, var_count, sp, argc, vidx)) {
                            if (whale_dbg_env("WHALE_JIT_DUMP"))
                                fprintf(stderr, "[vr-fuse] pos=%d argc=%d src=%d upd_col=%d\n",
                                        pos, argc, whale_fuse_srcslot,
                                        whale_fuse_atoms[whale_fuse_upd]);
                            whale_fuse_emit(&sp, cur, var_count, func_slot, fm,
                                            vidx, whale_vr_expect_slot[vidx],
                                            &whale_fields);
                            break;
                        }
                        /* The callee's own field names, in definition order -
                           its object literal is built as a real struct with
                           these columns (the same thing the scan pass does
                           for a whole function). */
                        whale_inline_str_n = 0;
                        for (cpos = 0; cpos < clen; ) {
                            int cop = cpc[cpos];

                            csz = whale_op_size_of(cop);
                            if (csz <= 0 || cpos + csz > clen)
                                break;
                            if (cop == OP_define_field &&
                                whale_inline_str_n < WHALE_ATOM_MAX)
                                whale_inline_str_atoms[whale_inline_str_n++] =
                                    (int)whale_get_u32(cpc + cpos + 1);
                            cpos += csz;
                        }
                        fm = fold_mark();   /* the stray argument tokens start here */
                        whale_inline_nested = 1;
                        rtype = whale_inline_body(&whale_vr_info[vidx], cur,
                                                  var_count, sp - argc);
                        whale_inline_nested = saved_nested;
                        if (whale_dbg_env("WHALE_JIT_DUMP"))
                            fprintf(stderr, "[vr-inline] pos=%d argc=%d rtype=%d clen=%d\n",
                                    pos, argc, rtype, whale_vr_info[vidx].byte_code_len);
                        if (rtype < 0) {
                            /* the inliner refused this callee: its statements
                               died with the body's buffer, so the truncate only
                               drops the stray argument tokens. Re-type the
                               callee as the plain parked value and take the
                               general path. */
                            JS_WhaleTokSinkTruncate(fm); whale_comma_open = 0;
                            cur[var_count + func_slot] =
                                WHALE_VAL_T(whale_vr_slot[vidx]);
                        } else {
                            /* the guard can only be committed once the body is
                               known to inline - a committed statement cannot be
                               taken back on refusal - and it has to precede
                               the body in the output, so: guard first, then
                               the buffered body. */
                            int gfm = fold_mark();

                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleVarRefIsFn");
                            wt('('); wt_id("ctx"); wt(','); wt_id("var_refs"); wt(',');
                            wt_int(vidx); wt(','); wt_int(whale_vr_expect_slot[vidx]); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                            fold_commit(gfm);
                            whale_inline_out_append();
                            cur[var_count + func_slot] = (uint8_t)rtype;
                            sp = func_slot + 1;
                            break;
                        }
                    } else {
                        cur[var_count + func_slot] =
                            WHALE_VAL_T(whale_vr_slot[vidx]);
                    }
                }
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
                        if (whale_dbg_env("WHALE_JIT_DUMP"))
                            fprintf(stderr, "[inline] pos=%d argc=%d rtype=%d clen=%d (num=%d slot=%d obj=%d)\n",
                                    pos, argc, rtype, ci.byte_code_len, rtype == WHALE_T_NUM,
                                    WHALE_IS_SLOT(rtype), WHALE_IS_OBJ(rtype));
                        if (rtype < 0) {
                            /* the inliner could not expand this callee - it has
                               locals, or holds something the inliner does not
                               handle. Throw away whatever it managed to emit and
                               take the general call path instead of abandoning the
                               whole function. */
                            JS_WhaleTokSinkTruncate(fm); whale_comma_open = 0;
                            goto whale_call_fallback;
                        }
                        /* the body committed into its own buffer; appending it
                           here replaces the fold_commit that used to re-copy the
                           whole workspace range - body tokens twice, plus stray
                           argument expressions the parser cannot take. */
                        whale_inline_out_append();
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
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                /* A laid-out object used as an argument has to become a real
                   JSValue before anything else looks at the arguments: both routes
                   through this instruction - the general call and the
                   self-recursion one - hand the operands to a compiled body, and a
                   body outside this compilation sees a heap object with a shape,
                   not the caller's frame columns.

                   Putting this inside the general-call branch covered only half the
                   cases: the self-recursion path then refused the struct argument
                   with its own bare "every argument must be a number" test, which is
                   what kept immutable_update out. */
                if (whale_str_n > 0) {
                    fm = fold_mark();
                    for (k = 0; k < argc; k++) {
                        int tk = cur[var_count + sp - argc + k];
                        int ms, m;

                        if (whale_dbg_env("WHALE_JIT_DUMP"))
                            fprintf(stderr, "[mat-arg] pos=%d k=%d tk=%d struct=%d strn=%d\n",
                                    pos, k, tk, WHALE_IS_STRUCT(tk), whale_str_n);
                        if (!WHALE_IS_STRUCT(tk))
                            continue;
                        if (whale_dbg_env("WHALE_JIT_DUMP"))
                            fprintf(stderr, "[mat-arg] objs=%d callbase=%d\n",
                                    whale_objs, WHALE_CALL_BASE);
                        if (whale_str_obj_slot < 0) {
                            if (whale_objs >= WHALE_CALL_BASE)
                                { WHALE_STEP_INSTEAD_OF_BAILING(); }
                            whale_str_obj_slot = whale_objs++;
                        }
                        ms = whale_str_obj_slot;
                        wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjNew"); wt('(');
                        wt_id("ctx"); wt(','); wt_int(ms); wt(')'); wt(')');
                        wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        for (m = 0; m < whale_str_n; m++) {
                            wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleObjDefineNum");
                            wt('('); wt_id("ctx"); wt(','); wt_int(ms); wt(',');
                            wt_int(whale_str_atoms[m]); wt(',');
                            wt_slot("L", whale_str_L[m]); wt(')'); wt(')');
                            wt('{'); wt('*'); wt_id("deopt"); wt('='); wt_int(1); wt(';');
                            wk(JS_WK_RETURN); wt_int(0); wt(';'); wt('}');
                        }
                        cur[var_count + sp - argc + k] = WHALE_OBJ_T(ms);
                    }
                    fold_commit(fm);
                }
                /* A self-call whose arguments are all numbers takes the self-recursion
                   path below: arguments into a double array, straight into the compiled
                   core. One that passes a parked object cannot - a double array has no
                   room for it - so those go through the general route, which now also
                   covers calling this function's own entry. */
                whale_call_obj_arg = 0;
                for (k = 0; k < argc; k++) {
                    int tk0 = cur[var_count + sp - argc + k];

                    if (!whale_type_is_num(tk0))
                        whale_call_obj_arg = 1;
                }
                if (cur[var_count + func_slot] != WHALE_T_SELF || whale_call_obj_arg) {
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

                    if (whale_dbg_env("WHALE_JIT_DUMP"))
                        fprintf(stderr, "[call] pos=%d argc=%d tc=%d slot=%d callslots=%d\n",
                                pos, argc, tc, WHALE_IS_SLOT(tc), WHALE_CALL_SLOTS);
                    if (!WHALE_IS_SLOT(tc) && tc != WHALE_T_SELF)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    if (argc > WHALE_CALL_SLOTS)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    /* Either the result is dead - the bytecode drops it right
                       after - or it is parked in a slot as an opaque value. A
                       result that is neither is refused: needing a number from
                       it later would deopt, and re-running this frame would run
                       the call a second time. */
                    if (nxt < len && (int)pc[nxt] == OP_drop) {
                        ret_slot = -1;
                    } else {
                        if (whale_objs >= WHALE_CALL_BASE)
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
                        ret_slot = whale_objs++;
                    }
                    /* The arguments were materialised above, before this branch. */
                    for (k = 0; k < argc; k++) {
                        int tk = cur[var_count + sp - argc + k];

                        if (!whale_type_is_num(tk) && !WHALE_IS_VAL(tk) && !WHALE_IS_OBJ(tk)) {
                            if (whale_dbg_env("WHALE_JIT_DUMP"))
                                fprintf(stderr, "[call] arg k=%d tk=%d rejected\n", k, tk);
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                    wk(JS_WK_IF); wt('('); wt('!');
                    if (tc == WHALE_T_SELF) {
                        /* WHALE_T_SELF is not a slot, so there is no JSValue to look up
                           in the frame; the helper takes the function from the frame
                           itself. */
                        wt_id("JS_WhaleCallSelfValue"); wt('('); wt_id("ctx"); wt(',');
                    } else {
                        wt_id("JS_WhaleCallValue"); wt('('); wt_id("ctx"); wt(',');
                        wt_int(WHALE_IS_VAL(tc) ? WHALE_VAL_SLOT(tc) : WHALE_OBJ_SLOT(tc)); wt(',');
                    }
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
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                       instead of as the call result. The call result itself
                       is a fresh value - whatever invariance the old expression
                       had is gone with it. */
                    whale_vname[func_slot] = nm;
                    whale_vmat[func_slot] = 1;
                    whale_vexpr[func_slot].inv = 0;
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (!whale_type_is_num(ta) || !whale_type_is_num(tb)) {
                    /* One operand is opaque - a string, a null, a value from a call.
                       That used to abandon the whole function; the engine can do the
                       operation properly, so hand it over and carry on. This is what
                       lets a vnode record, whose key is a string, stay compiled. */
                    {
                        int _okv;

                        WHALE_EMIT_BINOP_VAL(op, ta, tb, sp - 2, sp - 1, _okv);
                        if (!_okv)
                            { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    }
                    sp--;
                    break;
                }
                if (is_cmp) {
                    int btgt = 0, bis_true = 0, bisz = 0;

                    /* Compare-into-branch: `cmp; if_false/if_true` comes out as
                       one `if (!(a<b)) goto` instead of a boolean temporary
                       plus a test against zero. The temporary costs a statement
                       (spilled by TinyCC at every statement boundary) on every
                       iteration; the fused form costs the comparison alone.
                       `!(...)` rather than a flipped operator: for NaN the two
                       differ (`!(NaN<1)` is true, `NaN>=1` is false), and the
                       unfused form computes the former. Deferred locals are
                       flushed first - the branch leaves this position, and a
                       pending counter update would otherwise never be stored
                       (the loop would not terminate).
                       State machine mirrors the unfused two steps exactly (a
                       BOOL slot is recorded and then consumed, two pops): only
                       the emitted shape differs, so jump-target merges and
                       block-boundary materialisation see what they always saw.
                       That mirroring is load-bearing - skipping the fold_set
                       left stale slot types behind and broke objshape's
                       escaped phase with an empty `v[] = ()` store. */
                    if (whale_next_is_branch(pc, len, pos, sz, &btgt, &bis_true, &bisz)) {
                        int fm, cf;
                        int inva, invb;

                        local_flush_all(var_count);
                        inva = whale_vexpr[sp - 2].inv;
                        invb = whale_vexpr[sp - 1].inv;
                        fm = fold_mark();
                        wt('('); fold_inline(sp - 2); wt_op(opid); fold_inline(sp - 1); wt(')');
                        fold_set(sp - 2, fm, inva && invb);
                        cur[var_count + sp - 2] = WHALE_T_BOOL;
                        sp--;
                        cf = fold_mark();
                        wk(JS_WK_IF); wt('(');
                        if (!bis_true)
                            wt('!');
                        wt('('); fold_inline(sp - 1); wt(')');
                        wt(')'); wk(JS_WK_GOTO); wt_slot("L", btgt); wt(';');
                        fold_commit(cf);
                        sp--;
                        if ((btgt >= 0) && (btgt <= len) && (!seen[btgt])) {
                            cur[state_len - 1] = (uint8_t)sp;
                            seen[btgt] = 1;
                            memcpy(snap + (size_t)btgt * state_len, cur, state_len);
                        }
                        pos += sz + bisz;
                        continue;
                    }
                    {
                        int fm = fold_mark();
                        int inva, invb;

                        inva = whale_vexpr[sp - 2].inv;
                        invb = whale_vexpr[sp - 1].inv;
                        wt('('); fold_inline(sp - 2); wt_op(opid); fold_inline(sp - 1); wt(')'); wt('?'); wt_int(1); wt(':'); wt_int(0);
                        fold_set(sp - 2, fm, inva && invb);
                        cur[var_count + sp - 2] = WHALE_T_BOOL;
                    }
                } else {
                    int fm = fold_mark();
                    int inva, invb;

                    inva = whale_vexpr[sp - 2].inv;
                    invb = whale_vexpr[sp - 1].inv;
                    fold_inline(sp - 2); wt_op(opid); fold_inline(sp - 1);
                    fold_set(sp - 2, fm, inva && invb);
                    cur[var_count + sp - 2] = WHALE_T_NUM;
                }
                sp--;
            }
            break;
        case OP_mod:
            ta = cur[var_count + sp - 2];
            tb = cur[var_count + sp - 1];
            if (!whale_type_is_num(ta) || !whale_type_is_num(tb))
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
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

                /* Both sides already int32 with a nonzero constant divisor: a
                   bare C remainder is the whole operation. C's % takes the
                   dividend's sign exactly like JS, both sides are in int range
                   so there is no UB (INT_MIN % -1 needs a -1 divisor, and only
                   positive constants qualify), and the fmod fallback is dead -
                   so unlike fold_mod this emits no guard at all. This is the
                   shape `v % 7` has when v is a bitwise result. */
                if (pow2 == 0 &&
                    (ta == WHALE_T_I32 || ta == WHALE_T_ICONST ||
                     ta == WHALE_T_IBIT) &&
                    tb == WHALE_T_ICONST && !whale_vmat[sp - 1] &&
                    whale_vexpr[sp - 1].len == 1) {
                    double bv = gw_num[whale_vexpr[sp - 1].off];

                    if (bv > 0 && bv <= 2147483647.0 && bv == (double)(int)bv) {
                        int fm = fold_mark();
                        int inva = whale_vexpr[sp - 2].inv;

                        wt('('); wk(JS_WK_DOUBLE); wt(')');
                        wt('('); wt('('); wk(JS_WK_INT); wt(')');
                        if (ta == WHALE_T_ICONST && !whale_vmat[sp - 2] &&
                            whale_vexpr[sp - 2].len == 1)
                            wt_int((int)gw_num[whale_vexpr[sp - 2].off]);
                        else {
                            wt('('); fold_inline(sp - 2); wt(')');
                        }
                        wt('%'); wt_int((int)bv); wt(')');
                        fold_set(sp - 2, fm, inva);
                        cur[var_count + sp - 2] = WHALE_T_I32;
                        sp--;
                        break;
                    }
                }

                /* The form fold_mod emits mentions each operand up to four times
                   (two range tests, the fast path, the fmod fallback), so a long
                   pending expression gets a name first: inlining it into all
                   those copies copies it, copies of copies included. One `%` in
                   a loop generated kilobytes of C that way. A short operand (a
                   variable or a few tokens) inlines directly - naming it first
                   only adds a pure-copy statement (`v[n] = (L1)`) per % per
                   iteration. This has to happen before fold_mark():
                   materialising appends statements, and anything inside the
                   marked range would be treated as part of the expression. */
                if (!whale_vmat[sp - 2] && whale_vexpr[sp - 2].len > 8)
                    fold_materialize(sp - 2);
                if (!whale_vmat[sp - 1] && whale_vexpr[sp - 1].len > 8)
                    fold_materialize(sp - 1);
                fm = fold_mark();
                {
                    int inva = whale_vexpr[sp - 2].inv;
                    int invb = whale_vexpr[sp - 1].inv;

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
                    fold_set(sp - 2, fm, inva && invb);
                }
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
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            /* Bare-int production: when both operands are int-clean the C
               operands are already ints (fold_toint32 of I32/ICONST is a cast,
               of IBIT nothing at all), so the `(double)` wrapper is pure
               noise - skip it and mark the result IBIT. A NUM operand keeps
               the old wrapped form (I32): its `(long long)` truncation is
               semantically load-bearing. */
            if (whale_type_is_intclean(ta) && whale_type_is_intclean(tb)) {
                int fm = fold_mark();
                int inva = whale_vexpr[sp - 2].inv;
                int invb = whale_vexpr[sp - 1].inv;

                wt('(');
                fold_toint32(sp - 2, cur[var_count + sp - 2]);
                wt(op == OP_and ? '&' : op == OP_or ? '|' : '^');
                fold_toint32(sp - 1, cur[var_count + sp - 1]);
                wt(')');
                fold_set(sp - 2, fm, inva && invb);
                cur[var_count + sp - 2] = WHALE_T_IBIT;
                sp--;
                break;
            }
            {
                int fm = fold_mark();
                int inva = whale_vexpr[sp - 2].inv;
                int invb = whale_vexpr[sp - 1].inv;

                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('(');
                fold_toint32(sp - 2, cur[var_count + sp - 2]);
                wt(op == OP_and ? '&' : op == OP_or ? '|' : '^');
                fold_toint32(sp - 1, cur[var_count + sp - 1]);
                wt(')');
                fold_set(sp - 2, fm, inva && invb);
            }
            cur[var_count + sp - 2] = WHALE_T_I32;   /* & | ^ are int32 by definition */
            sp--;
            break;
        case OP_shl: case OP_shr: case OP_sar:
            ta = cur[var_count + sp - 2];
            tb = cur[var_count + sp - 1];
            if (!whale_type_is_num(ta) || !whale_type_is_num(tb))
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            /* Bare-int production for `<<`/`>>` (never `>>>`: unsigned, may
               exceed int32). Same deal as `& | ^` above. */
            if (op != OP_shr && whale_type_is_intclean(ta) &&
                whale_type_is_intclean(tb)) {
                int fm = fold_mark();
                int inva = whale_vexpr[sp - 2].inv;
                int invb = whale_vexpr[sp - 1].inv;

                wt('(');
                fold_toint32(sp - 2, cur[var_count + sp - 2]);
                wt_op(op == OP_shl ? JS_OP_SHL : JS_OP_SAR);
                wt('('); fold_toint32(sp - 1, cur[var_count + sp - 1]); wt('&'); wt_int(31); wt(')');
                wt(')');
                fold_set(sp - 2, fm, inva && invb);
                cur[var_count + sp - 2] = WHALE_T_IBIT;
                sp--;
                break;
            }
            {
                int fm = fold_mark();
                int inva = whale_vexpr[sp - 2].inv;
                int invb = whale_vexpr[sp - 1].inv;

                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('(');
                if (op == OP_shr) { wk(JS_WK_UNSIGNED); wk(JS_WK_INT); wt(')'); }   /* >>> is logical */
                fold_toint32(sp - 2, cur[var_count + sp - 2]);
                /* one token, not two '<' - there is no lexer to join them */
                wt_op(op == OP_shl ? JS_OP_SHL : JS_OP_SAR);
                wt('('); fold_toint32(sp - 1, cur[var_count + sp - 1]); wt('&'); wt_int(31); wt(')');
                wt(')');
                fold_set(sp - 2, fm, inva && invb);
            }
            /* >>> is unsigned, so its result can exceed int32; << and >> cannot */
            cur[var_count + sp - 2] = (op == OP_shr) ? WHALE_T_NUM : WHALE_T_I32;
            sp--;
            break;
        case OP_neg:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            {
                int fm = fold_mark();
                int inv = whale_vexpr[sp - 1].inv;

                wt('-'); fold_inline(sp - 1);
                fold_set(sp - 1, fm, inv);
            }
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        case OP_plus:
            /* +x on a number is the identity */
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        /* QuickJS has two different operators here: `lnot` is the logical
           `!x`, while `not` is the bitwise `~x`. Treating `not` as `!x` made
           every `~x` in a JIT'd function return 0 or 1. */
        case OP_lnot:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            {
                int fm = fold_mark();
                int inv = whale_vexpr[sp - 1].inv;
                int topt = cur[var_count + sp - 1];

                /* No branch fusion for `!x`: the int32 single-comparison form
                   already removed the NaN arm at the lnot itself, so a fused
                   branch would emit exactly what the unfused path does. */
                /* !x is true for 0 and for NaN. An int32 is never NaN, so a
                   known-int32 operand folds to a single comparison (and stops
                   tripling the operand's tokens - the untyped form names it
                   three times). IBIT rides along: a bare int is never NaN. */
                if (topt == WHALE_T_I32 || topt == WHALE_T_ICONST ||
                    topt == WHALE_T_IBIT) {
                    wt('('); fold_inline(sp - 1); wt_op(JS_OP_EQ); wt_int(0); wt(')');
                    wt('?'); wt_int(1); wt(':'); wt_int(0);
                } else {
                    wt('('); wt('('); fold_inline(sp - 1); wt_op(JS_OP_NE); fold_inline(sp - 1); wt(')');
                    wt_op(JS_OP_OR); wt('('); fold_inline(sp - 1); wt_op(JS_OP_EQ); wt_int(0); wt(')'); wt(')');
                    wt('?'); wt_int(1); wt(':'); wt_int(0);
                }
                fold_set(sp - 1, fm, inv);
            }
            cur[var_count + sp - 1] = WHALE_T_BOOL;
            break;
        case OP_not:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            /* Bare-int `~x` when the operand is int-clean (IBIT needs no
               conversion at all). Otherwise the wrapped form as before. */
            if (whale_type_is_intclean(cur[var_count + sp - 1])) {
                int fmn = fold_mark();
                int invn = whale_vexpr[sp - 1].inv;

                wt('~'); fold_toint32(sp - 1, cur[var_count + sp - 1]);
                fold_set(sp - 1, fmn, invn);
                cur[var_count + sp - 1] = WHALE_T_IBIT;
                break;
            }
            {
                int fm = fold_mark();
                int inv = whale_vexpr[sp - 1].inv;

                wt('('); wk(JS_WK_DOUBLE); wt(')'); wt('~'); fold_toint32(sp - 1, cur[var_count + sp - 1]);
                fold_set(sp - 1, fm, inv);
            }
            cur[var_count + sp - 1] = WHALE_T_I32;
            break;
        case OP_inc: case OP_dec:
            if (!whale_type_is_num(cur[var_count + sp - 1]))
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            {
                int fm = fold_mark();
                int inv = whale_vexpr[sp - 1].inv;

                fold_inline(sp - 1); wt(op == OP_inc ? '+' : '-'); wt_int(1);
                fold_set(sp - 1, fm, inv);
            }
            cur[var_count + sp - 1] = WHALE_T_NUM;
            break;
        case OP_inc_loc:
            {
                int idx = (int)pc[pos + 1];
                int fm;

                if (idx < 0 || idx >= var_count || !whale_type_is_num(cur[idx]))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                /* Int-region induction step: `I<g>++`, no conversion, no
                   deferred expression (the mirror is always a plain
                   variable). Type stays I32 - the value cannot leave int32:
                   the entry guard capped the trip count at 2^30. */
                if (whale_int_ok && idx >= 0 && idx < WHALE_JIT_MAX_SLOTS &&
                    whale_int_mirror[idx] && whale_loop_depth > 0 &&
                    idx == whale_int_g) {
                    local_materialize(idx);
                    fm = fold_mark();
                    /* `I<g> = I<g> + 1`, not `I<g>++`: the token stream has
                       no `++` token (two separate `+` pushes parse as binary
                       plus then unary plus, i.e. `expected expression`), and
                       a new operator id is not worth it for one site. */
                    wt_slot("I", idx); wt('=');
                    wt_slot("I", idx); wt('+'); wt_int(1);
                    wt(';');
                    fold_commit(fm);
                    cur[idx] = WHALE_T_I32;
                    whale_lo_c[idx] = 0;
                    break;
                }
                /* `L++` folds like add_loc: the current value may still be a
                   pending expression, so it is written out first. The result
                   names the variable L<idx> - never invariant. */
                local_materialize(idx);
                fm = fold_mark();
                wt_slot("L", idx); wt('+'); wt_int(1);
                local_set(idx, fm, 0);
                cur[idx] = WHALE_T_NUM;
                whale_lo_c[idx] = 0;
            }
            break;
        case OP_drop:
            if (sp <= 0)
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            /* the value just disappears; its expression is never inlined and
               the slot is overwritten by the next push */
            sp--;
            break;
        case OP_dup:
            if (sp <= 0)
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
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
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
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

        /* ---- exceptions -------------------------------------------------------
           The handler is a compile-time stack: `catch` says where it goes and
           `nip_catch` says when it stops being active. Nothing here touches the
           normal path - a thrown value is the only thing that ever consults it. */
        case OP_catch:
            {
                int tgt = pos + 1 + (int32_t)whale_get_u32(pc + pos + 1);

                if (whale_catch_depth >= WHALE_CATCH_MAX)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                whale_catch_tgt[whale_catch_depth++] = tgt;
                /* The catch block expects the exception value where it begins. On
                   the path that reaches it without throwing, that is undefined; the
                   throw path overwrites the same slot with the real value. */
                if (whale_objs >= WHALE_CALL_BASE)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                {
                    int os = whale_objs++;
                    int fm = fold_mark();

                    wt_id("JS_WhaleObjSlotUndef"); wt('('); wt_id("ctx"); wt(',');
                    wt_int(os); wt(')'); wt(';');
                    /* fold_set, not fold_commit: the slot's fold state has to say it
                       is materialised, or a later op inlining stack slot `sp` gets an
                       empty expression - `v[0] = () ;`, which TinyCC refuses.
                       An object slot, never a number: invariance is moot. */
                    fold_set(sp, fm, 0);
                    cur[var_count + sp] = WHALE_VAL_T(os);
                }
                sp++;
            }
            break;
        case OP_nip_catch:
            if (whale_catch_depth <= 0)
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            whale_catch_depth--;
            /* pops two, pushes one: the exception goes, the value that was above it
               drops into its place. Just decrementing `sp` leaves the exception in
               the slot the code above reads - which is how `return x*2` inside a
               `try` came back as the catch block's undefined. */
            if (sp < 2)
                { WHALE_STEP_INSTEAD_OF_BAILING(); }
            cur[var_count + sp - 2] = cur[var_count + sp - 1];
            whale_vexpr[sp - 2] = whale_vexpr[sp - 1];
            whale_vmat[sp - 2] = whale_vmat[sp - 1];
            sp--;
            break;
        case OP_throw:
            {
                /* -1 when no handler is open: then the exception is not for this
                   function to handle, it leaves on the return path. */
                int tgt = (whale_catch_depth > 0)
                    ? whale_catch_tgt[whale_catch_depth - 1] : -1;

                ta = cur[var_count + sp - 1];
                if (!WHALE_IS_VAL(ta) && !WHALE_IS_OBJ(ta) && !whale_type_is_num(ta))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                {
                    int fm = fold_mark();

                    if (WHALE_IS_VAL(ta) || WHALE_IS_OBJ(ta)) {
                        /* the slot holds the value; JS_WhaleObjGet duplicates it */
                        wt_id("JS_WhaleThrow"); wt('('); wt_id("ctx"); wt(',');
                        wt_id("JS_WhaleObjGet"); wt('('); wt_id("ctx"); wt(',');
                        wt_int(WHALE_IS_VAL(ta) ? WHALE_VAL_SLOT(ta) : WHALE_OBJ_SLOT(ta));
                        wt(')'); wt(')'); wt(';');
                    } else {
                        wt_id("JS_WhaleThrow"); wt('('); wt_id("ctx"); wt(',');
                        wt_id("whale_jit_box_num"); wt('('); wt_id("ctx"); wt(',');
                        fold_inline(sp - 1); wt(')'); wt(')'); wt(';');
                    }
                    if (tgt >= 0) {
                        /* a handler is open: the exception continues there */
                        wk(JS_WK_GOTO); wt_slot("L", tgt); wt(';');
                    } else {
                        /* Nothing catches it in this function, so it leaves the way
                           any exception does. *deopt == 2 is the engine's "a call
                           inside the compiled body threw": the exception is already
                           pending in ctx and the frame must NOT run again, because
                           the throw may be a side effect (a DOM mutation). The value
                           is unused either way, and the core returns a double - so 0
                           is the only thing this can return. Returning a JSValue
                           here is what made TinyCC say "invalid aggregate type for
                           register load". */
                        wt('*'); wt_id("deopt"); wt('='); wt_int(2); wt(';');
                        wk(JS_WK_RETURN); wt_int(0); wt(';');
                    }
                    fold_commit(fm);
                }
                unreachable = 1;
                sp--;
            }
            break;
        case OP_return:
            {
                int fm;

                t = cur[var_count + sp - 1];
                if (whale_dbg_env("WHALE_OP_DBG"))
                    fprintf(stderr, "[ret] pos=%d sp=%d t=%d num=%d\n",
                            pos, sp, t, whale_type_is_num(t));
                if (WHALE_IS_OBJLIKE(t)) {
                    /* The slot holds the reference; hand a duplicate of it to
                       the caller through ret_obj so the two do not compete for
                       the same reference count. A function that returns both a
                       number and an object is refused - one return shape only.

                       WHALE_T_VAL belongs here because that is what a general
                       call's result is: `var v = f(); return v;` parks the
                       result in a slot as an opaque value, and refusing it here
                       is what made every function that returns a call's result
                       fall back to the interpreter.

                       WHALE_T_STRUCT belongs here for the same reason, and it was
                       spelled out as an enumeration of the other three instead of
                       a predicate, which is how it came to be missing: an object
                       this function laid out is still an object. `setField` in
                       immutable_update is `return { a: o.a, ... }`, and refusing
                       it left every caller of it on the interpreter. The slot is
                       encoded the same way for all four, so WHALE_OBJ_SLOT reads
                       it - and JS_WhaleObjGet hands the object back out of it. */
                    if (ret_is_bool != -1)
                        { WHALE_STEP_INSTEAD_OF_BAILING(); }
                    whale_ret_obj = 1;
                    fm = fold_mark();
                    wt('*'); wt_id("ret_obj"); wt('='); wt_id("JS_WhaleObjGet"); wt('(');
                    wt_id("ctx"); wt(','); wt_int(WHALE_SLOT(t)); wt(')'); wt(';');
                    wk(JS_WK_RETURN); wt_int(0); wt(';');
                    fold_commit(fm);
                    unreachable = 1;
                    break;
                }
                if (!whale_type_is_num(t))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); }
                if (whale_ret_obj)
                    { WHALE_STEP_INSTEAD_OF_BAILING(); } /* a numeric return after an object one: mixed */
                if (ret_is_bool == -1)
                    ret_is_bool = (t == WHALE_T_BOOL);
                else if (ret_is_bool != (t == WHALE_T_BOOL))
                    { WHALE_STEP_INSTEAD_OF_BAILING(); } /* mixed number/boolean returns: not our shape */
                fm = fold_mark();
                /* the return closes any open store sequence first. */
                whale_comma_close();
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
            { whale_out_line = __LINE__; goto out; } /* anything else: keep interpreting */
        }
        whale_next_op:
        pos += sz;

        /* the back edge of the innermost open loop closes its buffer. `pos`
           already advanced past the jump, so compare against where it was
           (`pos - sz` is the jump itself). Comparing `pos` itself never
           fires (a 2-byte goto leaves pos 2 past the back edge) - which used
           to leave every loop buffer open until end of function, silently
           disabling anything that reads a closed body (unrolling). */
        if (whale_loop_depth > 0 &&
            pos - sz == whale_loops[whale_loop_depth - 1].back_pos)
            loop_end();
    }

    /* a loop without a back edge (a `for(;;)` whose exit is a throw, say) would
       leave its buffer open - flush whatever is left before committing */
    while (whale_loop_depth > 0)
        loop_end();
    /* function end closes any open store sequence. */
    whale_comma_close();

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
#ifdef WHALE_DEBUG
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
#endif
    for (i = 0; i < arg_count; i++) {
        if (!args_used[i])
            continue;
        if (arg_obj_slot[i] >= 0) {
            /* travels as an object (or a packed array), so it is not unboxed
               as a number. An argument the body indexes is bound as an array,
               which checks the class as well - a wrong guess only deopts. */
            if (args_arr[i]) {
                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleArrBind"); wt('('); wt_id("ctx"); wt(',');
                wt_int(arg_obj_slot[i]); wt(','); wt_id("args"); wt(','); wt_int(i); wt(')'); wt(')');
            } else {
                wk(JS_WK_IF); wt('('); wt('!'); wt_id("JS_WhaleArgGetObj"); wt('('); wt_id("ctx"); wt(',');
                wt_id("args"); wt('['); wt_int(i); wt(']'); wt(','); wt_int(arg_obj_slot[i]); wt(')'); wt(')');
            }
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
    /* Report how the call ended, before the flag is acted on. A non-zero flag here
       means the caller drops this result and interprets the frame, so a function
       that always reports one is compiled but never run - see JS_WhaleDeoptNote. */
    wt_id("JS_WhaleDeoptNote"); wt('('); wt_id("ctx"); wt(','); wt('*'); wt_id("deopt"); wt(')'); wt(';');
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
        wk(JS_WK_RETURN); wt_id("JS_WhaleNewBool"); wt('('); wt_id("ctx"); wt(','); wt_id("_r"); wt_op(JS_OP_NE); wt_int(0); wt(')'); wt(';');
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
    if (!ret && whale_dbg_env("WHALE_JIT_DUMP")) {
        int bad = (pos < len) ? pc[pos] : -1;

        fprintf(stderr, "whale jit: gen gave up at pos=%d op=%d (%s) bail=%d\n",
                pos, bad, whale_op_name(bad), whale_out_line);
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
    if (args_arr)
        free(args_arr);
    if (arg_obj_slot)
        free(arg_obj_slot);
    if (loc_slot)
        free(loc_slot);
    if (loop_back)
        free(loop_back);
    if (whale_loop_trip)
        free(whale_loop_trip);
    if (whale_loop_trip_g)
        free(whale_loop_trip_g);
    if (whale_loop_trip_exit)
        free(whale_loop_trip_exit);
    whale_loop_trip = NULL;
    whale_loop_trip_g = NULL;
    whale_loop_trip_exit = NULL;
    if (whale_loop_memset)
        free(whale_loop_memset);
    whale_loop_memset = NULL;
    if (whale_loop_countrec)
        free(whale_loop_countrec);
    whale_loop_countrec = NULL;
    if (whale_loop_intreg)
        free(whale_loop_intreg);
    whale_loop_intreg = NULL;
    if (whale_loop_reuse)
        free(whale_loop_reuse);
    whale_loop_reuse = NULL;
    if (whale_loop_stable)
        free(whale_loop_stable);
    whale_loop_stable = NULL;
    whale_loop_stable_tab = NULL;
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
    if (whale_loop_written)
        free(whale_loop_written);
    whale_loop_written = NULL;
    whale_loop_wn = 0;
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

typedef struct WhaleHostSym {
    const char *name;
    void       *addr;
} WhaleHostSym;

/* The one place that says which host functions generated code may call.
   TinyCC needs this map to resolve them, and a gcc backend needs exactly
   the same map to relocate the undefined symbols in the object file it
   gets back - so it is one table rather than sixty calls. Whether each of
   them also has a prototype in the token stream is a separate question,
   and the one an implicit declaration hides: a name that is registered
   here but never declared there compiles under TinyCC and does not under
   gcc, which is how JS_WhaleDeoptNote stayed missing. */
#define WHALE_HOST_SYM(fn) { #fn, (void *)fn }

static const WhaleHostSym whale_host_syms[] = {
    WHALE_HOST_SYM(JS_WhaleVarRefIsSelf),
    WHALE_HOST_SYM(JS_WhaleVarRefGetNum),
    WHALE_HOST_SYM(JS_WhaleVarRefSetNum),
    WHALE_HOST_SYM(JS_WhaleVarRefGetObj),
WHALE_HOST_SYM(JS_WhaleVarRefIsFn),
    WHALE_HOST_SYM(JS_WhaleVarRefPval),
    WHALE_HOST_SYM(JS_WhaleCheckStack),
    WHALE_HOST_SYM(whale_jit_box_num),
    WHALE_HOST_SYM(whale_jit_unbox_num),
    WHALE_HOST_SYM(JS_WhaleNewFloat64),
    WHALE_HOST_SYM(JS_WhaleNewBool),
    WHALE_HOST_SYM(JS_WhaleToInt32),
    WHALE_HOST_SYM(JS_WhaleDeoptNote),
    WHALE_HOST_SYM(JS_WhaleDeoptWhy),
    WHALE_HOST_SYM(JS_WhaleGetGlobalNum),
    WHALE_HOST_SYM(JS_WhaleObjSlot),
    WHALE_HOST_SYM(JS_WhaleObjGet),
    WHALE_HOST_SYM(JS_WhaleObjProbe),
    WHALE_HOST_SYM(JS_WhaleObjProbeFrom),
    WHALE_HOST_SYM(JS_WhaleGetGlobalObj),
    WHALE_HOST_SYM(JS_WhaleNewU8Array),
    WHALE_HOST_SYM(JS_WhaleTAU8Get),
    WHALE_HOST_SYM(JS_WhaleTAU8Set),
    WHALE_HOST_SYM(JS_WhaleTAU8Ptr),
    WHALE_HOST_SYM(JS_WhaleObjSlotNull),
    WHALE_HOST_SYM(JS_WhaleObjSlotEmptyString),
    WHALE_HOST_SYM(JS_WhaleObjSlotUndef),
    WHALE_HOST_SYM(JS_WhaleThrow),
    WHALE_HOST_SYM(JS_WhaleObjSlotAtom),
    WHALE_HOST_SYM(JS_WhaleObjDefineValue),
    WHALE_HOST_SYM(JS_WhaleBinopVal),
    WHALE_HOST_SYM(JS_WhaleCpoolSlot),
    WHALE_HOST_SYM(JS_WhaleObjStoreValue),
    WHALE_HOST_SYM(JS_WhaleShapeFields),
    WHALE_HOST_SYM(JS_WhaleObjNew),
    WHALE_HOST_SYM(JS_WhaleObjShape),
    WHALE_HOST_SYM(JS_WhaleObjFieldPtr),
    WHALE_HOST_SYM(JS_WhaleObjDefineNum),
    WHALE_HOST_SYM(JS_WhaleObjValueNum),
    WHALE_HOST_SYM(JS_WhaleArrPtr),
    WHALE_HOST_SYM(JS_WhaleArrNew),
    WHALE_HOST_SYM(JS_WhaleArrSetNum),
WHALE_HOST_SYM(JS_WhaleArrSetVal),
    WHALE_HOST_SYM(JS_WhaleObjFieldNum),
    WHALE_HOST_SYM(JS_WhaleStructNew),
WHALE_HOST_SYM(JS_WhaleStructDataPtr),
    WHALE_HOST_SYM(JS_WhaleStructFieldNum),
    WHALE_HOST_SYM(JS_WhaleStructSetNum),
    WHALE_HOST_SYM(JS_WhaleNativeEnsureSlot),
    WHALE_HOST_SYM(JS_WhaleNativeSlotFieldNum),
    WHALE_HOST_SYM(JS_WhaleNativeSlotPutNum),
    WHALE_HOST_SYM(JS_WhaleObjStoreNum),
    WHALE_HOST_SYM(JS_WhaleObjSlotNum),
    WHALE_HOST_SYM(JS_WhaleObjSlotCopy),
    WHALE_HOST_SYM(JS_WhaleArgGetObj),
WHALE_HOST_SYM(JS_WhaleArrBind),
    WHALE_HOST_SYM(JS_WhaleCallValue),
WHALE_HOST_SYM(JS_WhaleCallMethodValue),
    WHALE_HOST_SYM(JS_WhaleCallSelfValue),
    WHALE_HOST_SYM(JS_WhaleConstructValue),
    WHALE_HOST_SYM(JS_WhaleExceptionValue),
    WHALE_HOST_SYM(JS_WhaleStepPushNum),
    WHALE_HOST_SYM(JS_WhaleStepPushSlot),
    WHALE_HOST_SYM(JS_WhaleStepRun),
    WHALE_HOST_SYM(JS_WhaleStepOutIsNum),
    WHALE_HOST_SYM(JS_WhaleStepOutNum),
    WHALE_HOST_SYM(JS_WhaleStepOutSlot),
    WHALE_HOST_SYM(JS_WhaleStepClear),
    /* libc memset, for Uint8Array fill loops the generator lowers wholesale.
       Registered like every other host symbol; the generated code declares
       its own prototype. */
    { "memset", (void *)memset },
    WHALE_HOST_SYM(JS_WhaleTAU8CountNZ),
};
#define WHALE_HOST_SYM_COUNT \
    ((int)(sizeof(whale_host_syms) / sizeof(whale_host_syms[0])))

/* The token stream names identifiers by TinyCC's own ident index, so the ident
   table has to exist before anything can be rendered back to text - the gcc
   backend included. That is not a coupling that can be designed away: the table
   is where the names live. Created once and shared; the symbols registered with
   it are what the TinyCC path resolves generated calls against. */
static int whale_jit_state_ensure(void)
{
    TCCState *s;

    if (whale_jit_state)
        return 1;
    s = tcc_new();
    if (!s)
        return 0;
    tcc_set_lib_path(s, WHALE_JIT_TCC_DIR);
    tcc_add_include_path(s, WHALE_JIT_TCC_DIR "/include");
    tcc_add_include_path(s, WHALE_JIT_SRC_DIR);
    tcc_add_library_path(s, WHALE_JIT_TCC_DIR "/lib");
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    for (int i = 0; i < WHALE_HOST_SYM_COUNT; i++)
        tcc_add_symbol(s, whale_host_syms[i].name, whale_host_syms[i].addr);
    whale_jit_state = s;
    return 1;
}
static int whale_tcc_compile(JSContext *ctx, const char *src, void **entry_out, void **handle_out)
{
    TCCState *s;
    void *fn;

    if (whale_dbg_env("WHALE_JIT_DUMP"))
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
    if (!whale_jit_state_ensure())
        return 0;
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
        if (whale_dbg_env("WHALE_JIT_DUMP"))
            fprintf(stderr, "whale jit: compile failed (gen_ok=%d)\n", whale_gen_ok);
        goto fail;
    }
    /* Diagnostics: write the object out so the machine code TinyCC actually
       produced can be disassembled (objdump -d). Everything so far has been read
       from the generated C, which is one step removed from what runs.
       (Diagnostic: debug builds only.) */
#ifdef WHALE_DEBUG
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
#endif
    if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0)
        goto fail;
    fn = tcc_get_symbol(s, whale_entry_name);
    if (!fn)
        goto fail;

    if (whale_dbg_env("WHALE_JIT_DUMP"))
        fprintf(stderr, "whale jit: %s -> %p\n", whale_entry_name, fn);


    *entry_out = fn;
    *handle_out = s;
    JS_WhalePinLeave();
    return 1;
  fail:
    /* keep the state: it owns every previously compiled function's code, and
       tcc_delete() would release the global symbol pools they share */
    JS_WhalePinLeave();
    return 0;
}

/* ---- the gcc backend ------------------------------------------------------
   A second consumer of the same token stream. TinyCC parses those tokens inside
   this process; a gcc driver only reads text, so the stream is rendered with
   JS_WhaleRenderStream - the function the WHALE_JIT_SRC dump already uses - and
   handed to the driver's stdin pipe, which keeps the source in memory. What
   comes back is a shared object, and the loader loads that from a file, so it
   is written under %TEMP% for exactly as long as it takes to map, then removed.

   The host functions the generated code calls are resolved at link time, so the
   executable has to export them (whale_host_syms) and dlltool has to turn that
   export table into an import library. The library does not depend on the
   function being compiled, so it is built once and cached.
   ------------------------------------------------------------------------ */
/* Rendered on the TinyCC side, which is a separate translation unit: the token
   spellings are TinyCC's, so that is where the renderer lives. */
int JS_WhaleRenderStream(const int *tok, const double *num, int n, char **out);
void JS_WhaleRenderFree(char *p);
#define WHALE_CC_PATH 640

/* The backend driver the embedder chose with JS_WhaleSetJitCc, and its flags.
   Process-global on purpose: the JIT backend (and the TinyCC state beneath it)
   is process-global, so a per-context setting could not have two backends live
   at once anyway. Empty means "not chosen" - the TinyCC backend stays. */
static char whale_cc_driver[WHALE_CC_PATH];
static char whale_cc_flags[WHALE_CC_PATH];

void JS_WhaleSetJitCc(const char *driver, const char *flags)
{
    if (driver && *driver)
        snprintf(whale_cc_driver, sizeof(whale_cc_driver), "%s", driver);
    else
        whale_cc_driver[0] = '\0';
    if (flags && *flags)
        snprintf(whale_cc_flags, sizeof(whale_cc_flags), "%s", flags);
    else
        whale_cc_flags[0] = '\0';
}

static int whale_gcc_compile(JSContext *ctx, const char *src, void **entry_out, void **handle_out)
{
    const char *cc = whale_cc_driver;
    const char *tmp = getenv("TEMP");
    const char *extra;
    char *text = NULL;
    char obj[WHALE_CC_PATH], cmd[WHALE_CC_PATH * 4];
    TCCState *s;
    FILE *pipe;
    void *fn;
    int n, ok = 0;

    (void)src;
    if (!cc || !*cc)
        return 0;
    if (!tmp || !*tmp)
        return 0;
    /* The state and its host symbols: the object below references them, and
       pe_build_imports binds its undefined symbols to these addresses - the
       same mechanism the TinyCC path uses. No import library, no exports. */
    if (!whale_jit_state_ensure())
        return 0;
    s = whale_jit_state;

    snprintf(whale_core_name, sizeof(whale_core_name), "whale_jit_core_%u", ++whale_jit_seq);
    snprintf(whale_entry_name, sizeof(whale_entry_name), "whale_jit_fn_%u", whale_jit_seq);

    /* The generator alone: TinyCC's parser is not driven, because the consumer
       on this path reads text. The tokens are already in final order in
       go_tok/go_num when it returns. */
    if (!whale_jit_gen(ctx, whale_gen_info))
        return 0;
    n = JS_WhaleRenderStream(go_tok, go_num, go_len, &text);
    if (!text)
        return 0;
    if (n <= 0)
        goto done;

    /* gcc only compiles (`-c`): the object is linked by this process's own
       TinyCC state (pe_load_coff), never by an external linker, and there is
       no shared object, no export table, no LoadLibrary. The assembler cannot
       write to stdout, so the object takes a temporary file; the source still
       travels by pipe and is never written out.
       -fno-strict-aliasing is not optional: the generated code reaches through
       casts the compiler cannot see the provenance of, and -O2 is entitled to
       assume otherwise. TinyCC does not care, so this only shows up here.
       The flag override comes from JS_WhaleSetJitCc's second argument. */
    snprintf(obj, sizeof(obj), "%s\\whale_jit_%u.o", tmp, whale_jit_seq);
    DeleteFileA(obj);
    extra = whale_cc_flags[0] ? whale_cc_flags : NULL;
    if (!extra)
        extra = "";
    /* The outer quotes are for cmd.exe, which otherwise strips the driver's own
       quotes - same reason as the dlltool call used to need. */
    snprintf(cmd, sizeof(cmd),
             "\"\"%s\" -x c - -c -w -fno-strict-aliasing "
             "-Wno-error=incompatible-pointer-types %s -o \"%s\"\"",
             cc, *extra ? extra : "-O2", obj);
    if (whale_dbg_env("WHALE_JIT_DUMP"))
        fprintf(stderr, "whale gcc: %s\n", cmd);
    pipe = _popen(cmd, "w");
    if (!pipe) {
        if (whale_dbg_env("WHALE_JIT_DUMP"))
            fprintf(stderr, "whale gcc: cannot start the driver\n");
        goto done;
    }
    fwrite(text, 1, n, pipe);
    if (_pclose(pipe) != 0) {
        if (whale_dbg_env("WHALE_JIT_DUMP"))
            fprintf(stderr, "whale gcc: driver failed\n");
        goto done;
    }
    if (tcc_add_file(s, obj) < 0) {
        if (whale_dbg_env("WHALE_JIT_DUMP"))
            fprintf(stderr, "whale gcc: tcc cannot load %s\n", obj);
        goto done_unlink;
    }
    /* tcc_add_file does not go through JS_WhaleCompileTokens, whose epilogue
       clears runtime_main: without the same clearing the next pe_output_file
       treats the state as "run a program" and injects an unresolvable
       __runmain plus the CRT libraries. */
    JS_WhaleTccClearRuntimeMain(s);
    if (tcc_relocate(s, TCC_RELOCATE_AUTO) < 0) {
        if (whale_dbg_env("WHALE_JIT_DUMP"))
            fprintf(stderr, "whale gcc: relocate failed\n");
        goto done_unlink;
    }
    fn = tcc_get_symbol(s, whale_entry_name);
    if (!fn) {
        if (whale_dbg_env("WHALE_JIT_DUMP"))
            fprintf(stderr, "whale gcc: %s not found\n", whale_entry_name);
        goto done_unlink;
    }
    if (whale_dbg_env("WHALE_JIT_DUMP"))
        fprintf(stderr, "whale gcc: %s -> %p\n", whale_entry_name, fn);
    *entry_out = fn;
    /* the single TCCState is process-wide and must outlive the functions it
       hosts, same as the TinyCC path: no per-function handle to release */
    *handle_out = (void *)s;
    /* WHALE_JIT_KEEP holds the object back for a post-mortem: objdump -d on
       it is the only account of what gcc actually emitted. */
    if (whale_dbg_env("WHALE_JIT_KEEP")) {
        fprintf(stderr, "whale gcc: kept %s\n", obj);
    } else {
        DeleteFileA(obj);
    }
    ok = 1;
  done:
    JS_WhaleRenderFree(text);
    return ok;
  done_unlink:
    DeleteFileA(obj);
    goto done;
}

static void whale_gcc_release(void *handle)
{
    /* same as the TinyCC path: the state outlives every function in it */
    (void)handle;
}

static JSWhaleJitBackend whale_backend_gcc = {
    "gcc",
    whale_gcc_compile,
    whale_gcc_release,
};
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

    if (whale_dbg_env("WHALE_JIT_DUMP")) {
        /* there is no source text any more: the generator emits tokens straight
           into TinyCC. The probe only shows that we really are inside the
           TinyCC translation unit. */
        fprintf(stderr, "whale tcc internal probe: vstack cap=%d\n", JS_WhaleTccProbe());
    }

    /* gcc is opt-in twice over: the embedder names a driver with
       JS_WhaleSetJitCc, and marks the function with JS_WhaleSetJitCcForFunction.
       Everything else stays on TinyCC - no toolchain needed, no behaviour
       change - which is why this is per function and not a global switch.
       (The old import-library sentence is gone with the .dll path: the object
       is linked in-process, so there is nothing to build on first use.) */
    if (info->prefer_gcc && whale_cc_driver[0])
        whale_jit_backend = &whale_backend_gcc;
    else
        whale_jit_backend = &whale_backend_tcc;
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