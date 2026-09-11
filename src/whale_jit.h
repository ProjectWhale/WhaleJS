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
