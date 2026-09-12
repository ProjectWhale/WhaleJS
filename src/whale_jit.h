/*
 * WhaleJS JIT/AOT backend interface (implementation in whale_jit.c).
 *
 * The engine packs everything the code generator needs into JSWhaleFuncInfo, so
 * the generator never touches quickjs internals; the few internal services it
 * does need come through the JS_Whale* wrappers below (implemented by the
 * engine). Keeping the generator in its own translation unit means editing it
 * rebuilds a small file instead of all of quickjs.c.
 */
#ifndef WHALE_JIT_H
#define WHALE_JIT_H

#include "quickjs.h"

typedef struct JSWhaleFuncInfo {
    const uint8_t *byte_code;
    int byte_code_len;
    int is_normal;      /* func_kind == JS_FUNC_NORMAL */
    int arg_count;
    int var_count;
    int stack_size;
    int var_ref_count;
    int cpool_count;
    const JSValue *cpool;
} JSWhaleFuncInfo;

/* Number of JSValue slots the engine lends to a JIT-compiled function so it has
   somewhere GC can see an object it is holding across a helper call.

   This is not an optimisation, it is what makes objects possible in JIT code at
   all. The generated function is a plain C function: it has no JSStackFrame, GC
   does not scan the C stack, and so a `void *` in a C local is invisible to the
   collector - the object would be freed underneath it. The engine therefore
   reserves this many slots immediately after the JS operand stack, walks them
   as part of the frame, and hands the address to the JIT. The JIT puts object
   number k in slot k.

   Borrowing them costs nothing at run time: the JIT keeps every intermediate
   value in C doubles and never uses the JS operand stack, which is the region
   these slots sit right behind. */
#define WHALE_OBJ_SLOTS 8
/* object-property inline-cache pairs the generated code can declare */
#define WHALE_FIELD_CACHE 24
/* property names the generator can reason about (object literals only) */
#define WHALE_ATOM_MAX 32
/* direct calls the generator records, to see which loops contain one */
#define WHALE_CALL_MAX 32

/* Compile one function. Returns 1 and fills entry_out/handle_out on success;
   the caller owns both, and releases the handle with JS_WhaleJitRelease(). */
int  JS_WhaleJitCompileFunction(JSContext *ctx, const JSWhaleFuncInfo *info,
                                void **entry_out, void **handle_out);
void JS_WhaleJitRelease(void *handle);

/* services the engine provides to the generator */
int JS_WhaleVarRefIsSelf(JSContext *ctx, void **var_refs, int idx, JSValue self);
/* Closure slot read/write as a number. 0 to deopt: no binding, or a value that
   is not a number. This is what lets a function that captures an enclosing
   variable stay in the JIT instead of falling back to the interpreter. */
int JS_WhaleVarRefGetNum(JSContext *ctx, void *var_refs, int idx, double *out);
int JS_WhaleVarRefSetNum(JSContext *ctx, void *var_refs, int idx, double v);
int JS_WhaleCheckStack(JSContext *ctx);
int JS_WhaleOpSize(int op);   /* bytecode operand size, from the interpreter table */

/* Object slots: the GC-visible place for the JIT to keep an object across a
   helper call (see WHALE_OBJ_SLOTS above). The engine implements these against
   the calling frame, so the generated code does not have to pass an address. */
void JS_WhaleObjSlot(JSContext *ctx, int k, JSValue v);   /* park object in slot k */
JSValue JS_WhaleObjGet(JSContext *ctx, int k);            /* read it back */

/* Typed-array support. The generated code has no JSValue domain, so an array
   travels as an object slot number - a compile-time constant - and these do the
   object work. Each returns 0 to mean "deopt", and none may raise. */
int JS_WhaleGetGlobalObj(JSContext *ctx, int atom, int slot);
int JS_WhaleNewU8Array(JSContext *ctx, int ctor_slot, double n, int out_slot);
int JS_WhaleTAU8Get(JSContext *ctx, int slot, double idx, double *out);
int JS_WhaleTAU8Set(JSContext *ctx, int slot, double idx, double v);

/* Buffer base + element count for a Uint8Array in `slot`, so generated code can
   fetch both once and then index directly instead of calling an accessor per
   element. NULL means deopt. */
uint8_t *JS_WhaleTAU8Ptr(JSContext *ctx, int slot, double *len_out);

/* Plain-object fields, for the JIT's object-literal path. `JS_WhaleObjShape`
   gives an opaque key the generated code caches; `JS_WhaleObjFieldPtr` resolves
   a property against it and returns the address of its value, or NULL to deopt
   (wrong shape, missing property, or an accessor). */
int JS_WhaleObjNew(JSContext *ctx, int slot);
void *JS_WhaleObjShape(JSContext *ctx, int slot);
JSValue *JS_WhaleObjFieldPtr(JSContext *ctx, int slot, int atom, void **shape_io);
/* Define a numeric property, for an object literal's initialisers. */
int JS_WhaleObjDefineNum(JSContext *ctx, int slot, int atom, double v);

/* The cold branch of an inlined property read: sets *deopt and returns 0 when
   the value is neither an int32 nor a float64. */
double JS_WhaleObjValueNum(JSContext *ctx, JSValue *v, int *deopt);
/* Packed-array access: the backing store plus element count (NULL to deopt), a
   fresh empty array, and a numeric element store that appends. */
JSValue *JS_WhaleArrPtr(JSContext *ctx, int slot, double *len_out);
int JS_WhaleArrNew(JSContext *ctx, int slot);
int JS_WhaleArrSetNum(JSContext *ctx, int slot, double idx, double v);
/* Park argument `idx` in object slot `slot` as a packed array; 0 to deopt. Lets
   a function that takes an array stay in the JIT - the generated code assumes
   the parameter is one (a wrong guess only bails) and this checks it. */
int JS_WhaleArrBind(JSContext *ctx, int slot, JSValue *args, int idx);

/* Numeric property access with a shape cache held in the caller: *shape_cache
   and *ptr_cache are two opaque words the generated code declares. Sets *deopt
   and returns 0 when the shape changed under it, the property is missing, or
   the value is neither an int32 nor a float64. */
double JS_WhaleObjFieldNum(JSContext *ctx, int slot, int atom,
                           void **shape_cache, void **ptr_cache, int *deopt);
int JS_WhaleObjStoreNum(JSContext *ctx, int slot, int atom, double v,
                        void **shape_cache, void **ptr_cache);

/* Compile-time only - never called by generated code. Does global `atom` name
   the Uint8Array constructor? Asking while emitting is what keeps each stack
   slot's held type a single compile-time fact. */
int JS_WhaleGlobalIsU8Ctor(JSContext *ctx, int atom);

/* Test hook: force a GC, then report the .length of whatever is in slot k, or -1
   if the slot does not hold an object. It exists to prove the slots survive a
   collection, which is otherwise unobservable from JS. */
int JS_WhaleObjProbe(JSContext *ctx, int k);

/* Park a copy of v in slot 0, collect, then report its .length (-1 if v is not an
   object). Combined into one call so generated code never has to reference
   js_dup, which is static to the engine. */
int JS_WhaleObjProbeFrom(JSContext *ctx, JSValue v);

/* Keyword ids for the token emitters. The generator names a keyword; the
   TinyCC-side file maps it to its own TOK_*. ORDER MUST MATCH kwtok[] in
   src/tcc/whale_tcc_iface.c. */
enum {
    JS_WK_INT = 0, JS_WK_CHAR, JS_WK_VOID, JS_WK_LONG, JS_WK_UNSIGNED, JS_WK_FLOAT,
    JS_WK_DOUBLE, JS_WK_STATIC, JS_WK_EXTERN, JS_WK_TYPEDEF, JS_WK_STRUCT,
    JS_WK_RETURN, JS_WK_IF, JS_WK_ELSE, JS_WK_WHILE, JS_WK_FOR, JS_WK_DO,
    JS_WK_BREAK, JS_WK_CONTINUE, JS_WK_GOTO, JS_WK_SWITCH, JS_WK_CASE,
    JS_WK_DEFAULT, JS_WK_SIZEOF, JS_WK_REGISTER, JS_WK_UNION, JS_WK_ENUM, JS_WK_SHORT,
};

/* operator ids for JS_WhaleTokOp (same idea as the keyword ids) */
enum {
    JS_OP_ADD = 0, JS_OP_SUB, JS_OP_MUL, JS_OP_DIV,
    JS_OP_LT, JS_OP_LE, JS_OP_GT, JS_OP_GE, JS_OP_EQ, JS_OP_NE,
    JS_OP_OR, JS_OP_AND,
    /* These are single tokens in C, not two characters: the parser never sees
       a lexer, so `wt('<'); wt('<')` would arrive as two `<` and fail with
       "')' expected (got \"<\")". */
    JS_OP_SHL, JS_OP_SAR, JS_OP_SHR,
    /* member access, for inlined property reads: -> is one token in C */
    JS_OP_ARROW, JS_OP_DOT,
};

/* provided by src/tcc/whale_tcc_iface.c */
void JS_WhaleTokKw(int w);
void JS_WhaleTokIdent2(const char *name);
void JS_WhaleTokSlot(const char *prefix, int n);
void JS_WhaleTokInt(int v);
void JS_WhaleTokDouble(double v);
void JS_WhaleTokChar(int ch);
void JS_WhaleTokOp(int o);

#endif /* WHALE_JIT_H */
