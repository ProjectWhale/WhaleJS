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

/* Compile one function. Returns 1 and fills entry_out/handle_out on success;
   the caller owns both, and releases the handle with JS_WhaleJitRelease(). */
int  JS_WhaleJitCompileFunction(JSContext *ctx, const JSWhaleFuncInfo *info,
                                void **entry_out, void **handle_out);
void JS_WhaleJitRelease(void *handle);

/* services the engine provides to the generator */
int JS_WhaleVarRefIsSelf(JSContext *ctx, void **var_refs, int idx, JSValue self);
int JS_WhaleCheckStack(JSContext *ctx);
int JS_WhaleOpSize(int op);   /* bytecode operand size, from the interpreter table */

#endif /* WHALE_JIT_H */