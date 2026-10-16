/*
 * WhaleJS 对外接口单元测试。
 * 无框架：CHECK 失败即打印并让进程以非 0 退出。
 * 构建：xmake run whale_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"
#include "whalejs.h"
#include "libtcc.h"

/* iface helper: skip the CRT libraries for a state (every external the JIT
   needs is a host symbol, so no libtcc1-64.a has to exist) */
extern void JS_WhaleTccNoStdLibs(TCCState *s);

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

/* TCC 运行时支持目录（相对项目根；xmake run 的 cwd 就是项目根） */
#define WHALE_TCC_DIR "thirdparty/tcc-runtime"

/*
 * TCC 后端自检：证明 libtcc 已经链进来，且能在运行期把 C 源码编成机器码直接调用。
 * 这是 JIT 的地基——真正的字节码→C 翻译还没接。
 */
static void check_tcc_backend(void)
{
    TCCState *s = tcc_new();
    int (*add)(int, int);
    int (*fib)(int);
    int rc;

    CHECK(s != NULL);
    if (!s)
        return;

    /* Every external the JIT's generated code needs is a host symbol and the
       CRT libraries are skipped, so this check needs no on-disk tcc-runtime at
       all; WHALE_TCC_DIR is just a search path. */
    tcc_set_lib_path(s, WHALE_TCC_DIR);
    tcc_add_include_path(s, WHALE_TCC_DIR "/include");
    tcc_add_library_path(s, WHALE_TCC_DIR "/lib");
    /* the generated C here references no external at all (pure int code), so
       skipping the CRT is enough to make it compile with no tcc-runtime */
    JS_WhaleTccNoStdLibs(s);
    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

    rc = tcc_compile_string(s,
                            "int whale_add(int a, int b){ return a + b; }\n"
                            "int whale_fib(int n){ return n < 2 ? n : whale_fib(n-1) + whale_fib(n-2); }\n");
    CHECK(rc >= 0);
    if (rc < 0) {
        tcc_delete(s);
        return;
    }
    rc = tcc_relocate(s, TCC_RELOCATE_AUTO);
    CHECK(rc >= 0);
    if (rc < 0) {
        tcc_delete(s);
        return;
    }

    add = (int (*)(int, int))tcc_get_symbol(s, "whale_add");
    fib = (int (*)(int))tcc_get_symbol(s, "whale_fib");
    CHECK(add != NULL);
    CHECK(fib != NULL);
    if (add)
        CHECK(add(2, 3) == 5);
    if (fib)
        CHECK(fib(20) == 6765);

    tcc_delete(s);
}

/* ---- custom token stream driving TinyCC's own parser, no C source involved. */
extern void *JS_WhaleTokCompileAdd2(const char *tcc_dir);

static void check_ir_direct(void)
{
    double (*add2)(double, double);

    /* the runtime is all host symbols + embedded helpers now, so the probe's
       tcc_dir is just a search path; WHALE_TCC_DIR works with or without a
       directory behind it */
    add2 = (double (*)(double, double))JS_WhaleTokCompileAdd2(WHALE_TCC_DIR);

    printf("  [tok] token-stream compile: %s\n", add2 ? "compiled" : "NULL");
    if (add2) {
        double r = add2(2.5, 4.0);
        printf("        result = %.2f (expected 6.50)\n", r);
        CHECK(r == 6.5);
    }
}

/* ---- x86-64 direct emitter probe (route B skeleton): raw addsd bytes. */
extern void *JS_WhaleX64ProbeAdd2(void);

static void check_x64_probe(void)
{
    double (*add2)(double, double) = (double (*)(double, double))JS_WhaleX64ProbeAdd2();

    printf("  [x64] direct emit: %s\n", add2 ? "committed" : "NULL");
    if (add2) {
        double r = add2(2.5, 4.0);
        printf("        result = %.2f (expected 6.50)\n", r);
        CHECK(r == 6.5);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    check_ir_direct();
    check_x64_probe();

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

    /* 8. TCC 后端可用（JIT 地基） */
    /* 9. JIT: a hot non-recursive function must actually get compiled */
    JS_WhaleSetJitMode(ctx, JS_JIT_LAZY);
    {
        double v;
        clock_t t0;
        double ms;

        t0 = clock();
        v = eval_number(ctx,
            "function fibi(n){var a=0,b=1;for(var i=0;i<n;i++){var t=a+b;a=b;b=t;}return a;}"
            "var acc=0; for (var k=0;k<400;k++) acc=fibi(30); acc");
        ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        CHECK(v == 832040.0);
        JS_WhaleGetJitStats(ctx, &st);
        CHECK(st.compiled >= 1);
        printf("  [tcc] 400x fibi(30) = %.0f ms\n", ms);
    }

    /* 10. gcc backend: compile one marked function through gcc -c, linked
       in-process by TinyCC itself (no .dll, no LoadLibrary). Needs a MinGW
       gcc; the driver comes from WHALE_TEST_GCC or the well-known install
       path, and the test is skipped when neither exists. */
    {
        const char *gcc = getenv("WHALE_TEST_GCC");
        char gcc_path[512];
        FILE *probe;
        JSValue global, fn;
        uint32_t compiled_before;
        double v;

        if (!gcc || !*gcc) {
            snprintf(gcc_path, sizeof(gcc_path),
                     "C:/Program Files/mingw64/bin/gcc.exe");
            probe = fopen(gcc_path, "rb");
            if (probe)
                fclose(probe);
            else
                gcc_path[0] = '\0';
            gcc = gcc_path[0] ? gcc_path : NULL;
        }
        if (gcc) {
            clock_t t0;
            double ms;

            JS_WhaleSetJitCc(gcc, NULL);
            eval_number(ctx,
                "function fibg(n){var a=0,b=1;for(var i=0;i<n;i++){var t=a+b;a=b;b=t;}return a;} 0");
            global = JS_GetGlobalObject(ctx);
            fn = JS_GetPropertyStr(ctx, global, "fibg");
            JS_WhaleSetJitCcForFunction(ctx, fn);
            JS_FreeValue(ctx, fn);
            JS_FreeValue(ctx, global);
            JS_WhaleGetJitStats(ctx, &st);
            compiled_before = st.compiled;
            t0 = clock();
            v = eval_number(ctx,
                "var accg=0; for (var k=0;k<400;k++) accg=fibg(30); accg");
            ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
            CHECK(v == 832040.0);
            JS_WhaleGetJitStats(ctx, &st);
            CHECK(st.compiled > compiled_before);
            /* informational only: same shape as §9's fibi, for the notes */
            printf("  [gcc] 400x fibg(30) = %.0f ms (incl. one gcc compile)\n", ms);
            t0 = clock();
            v = eval_number(ctx,
                "var accg2=0; for (var k=0;k<400;k++) accg2=fibg(30); accg2");
            ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
            CHECK(v == 832040.0);
            printf("  [gcc] 400x fibg(30) again = %.0f ms (compiled code only)\n", ms);
            JS_WhaleSetJitCc(NULL, NULL);
        } else {
            printf("  [gcc] no driver found, skipped\n");
        }
    }

    check_tcc_backend();

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (failures) {
        printf("whale api test: %d FAILED\n", failures);
        return 1;
    }
    printf("whale api test: ok\n");
    return 0;
}
