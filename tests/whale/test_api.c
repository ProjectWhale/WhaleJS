/*
 * WhaleJS 对外接口单元测试。
 * 无框架：CHECK 失败即打印并让进程以非 0 退出。
 * 构建：xmake run whale_test
 */
#include <stdio.h>
#include <string.h>

#include "quickjs.h"
#include "whalejs.h"

static int failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* 求值一段必须返回数字的 JS；出错记为失败并返回 -1 */
static double eval_number(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<whale-test>", JS_EVAL_TYPE_GLOBAL);
    double d = -1.0;

    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        printf("FAIL eval 抛异常: %s -> %s\n", src, s ? s : "?");
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        failures++;
    } else if (!JS_IsNumber(v)) {
        printf("FAIL eval 结果不是数字: %s\n", src);
        failures++;
    } else {
        JS_ToFloat64(ctx, &d, v);
    }
    JS_FreeValue(ctx, v);
    return d;
}

/* 纯计算（热点候选）、控制流、字符串三条语义样本 */
#define SRC_FIB  "function fib(n){return n<2?n:fib(n-1)+fib(n-2)} fib(20)"
#define SRC_LOOP "var s=0; for (var i=0;i<1000;i++) s+=i; s"
#define SRC_STR  "('a'+'b'+[1,2,3].join('-')).length"

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    JSWhaleJitStats st;
    double fib_off, loop_off, str_off, fib_lazy, loop_lazy, str_lazy;
    uint32_t req_before;

    CHECK(rt != NULL);
    CHECK(ctx != NULL);
    if (!ctx) {
        printf("whale api test: cannot create context\n");
        return 1;
    }

    /* 1. 默认模式 = LAZY */
    CHECK(JS_WhaleGetJitMode(ctx) == JS_JIT_LAZY);

    /* 2. 模式读写往返 */
    JS_WhaleSetJitMode(ctx, JS_JIT_OFF);
    CHECK(JS_WhaleGetJitMode(ctx) == JS_JIT_OFF);
    JS_WhaleSetJitMode(ctx, JS_JIT_AOT);
    CHECK(JS_WhaleGetJitMode(ctx) == JS_JIT_AOT);
    JS_WhaleSetJitMode(ctx, JS_JIT_LAZY);
    CHECK(JS_WhaleGetJitMode(ctx) == JS_JIT_LAZY);

    /* 3. 统计初值为 0 */
    JS_WhaleGetJitStats(ctx, &st);
    CHECK(st.requests == 0 && st.compiled == 0 && st.failed == 0 && st.code_bytes == 0);

    /* 4. 统计可累加 */
    JS_WhaleNoteJitRequest(ctx);
    JS_WhaleNoteJitRequest(ctx);
    JS_WhaleNoteJitResult(ctx, 1, 128);
    JS_WhaleNoteJitResult(ctx, 0, 0);
    JS_WhaleGetJitStats(ctx, &st);
    CHECK(st.requests == 2);
    CHECK(st.compiled == 1);
    CHECK(st.failed == 1);
    CHECK(st.code_bytes == 128);

    /* 5. JIT=OFF：语义正确，且不产生 JIT 请求 */
    JS_WhaleSetJitMode(ctx, JS_JIT_OFF);
    JS_WhaleGetJitStats(ctx, &st);
    req_before = st.requests;
    fib_off = eval_number(ctx, SRC_FIB);
    loop_off = eval_number(ctx, SRC_LOOP);
    str_off = eval_number(ctx, SRC_STR);
    CHECK(fib_off == 6765.0);   /* fib(20) */
    CHECK(loop_off == 499500.0);
    CHECK(str_off == 7.0);      /* "ab1-2-3".length */
    JS_WhaleGetJitStats(ctx, &st);
    CHECK(st.requests == req_before);   /* OFF 下热点统计纹丝不动 */

    /* 6. JIT=LAZY：结果与 OFF 一致，且热点统计在涨（JIT 上线后这条是真正的回归检查） */
    JS_WhaleSetJitMode(ctx, JS_JIT_LAZY);
    JS_WhaleGetJitStats(ctx, &st);
    req_before = st.requests;
    fib_lazy = eval_number(ctx, SRC_FIB);
    loop_lazy = eval_number(ctx, SRC_LOOP);
    str_lazy = eval_number(ctx, SRC_STR);
    CHECK(fib_lazy == fib_off);
    CHECK(loop_lazy == loop_off);
    CHECK(str_lazy == str_off);
    JS_WhaleGetJitStats(ctx, &st);
    CHECK(st.requests > req_before);

    /* 7. NULL 入参不得崩 */
    JS_WhaleSetJitMode(NULL, JS_JIT_OFF);
    JS_WhaleGetJitStats(NULL, &st);
    CHECK(JS_WhaleGetJitMode(NULL) == JS_JIT_OFF);
    JS_WhaleNoteJitRequest(NULL);
    JS_WhaleNoteJitResult(NULL, 1, 1);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (failures) {
        printf("whale api test: %d FAILED\n", failures);
        return 1;
    }
    printf("whale api test: ok\n");
    return 0;
}
