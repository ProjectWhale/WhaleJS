/*
 * WhaleJS 嵌入示例 —— 只用安装出来的公开头文件（quickjs.h / whalejs.h）和库。
 *
 * 它同时是"库可用性"的回归验证：不碰任何引擎内部头（libtcc.h / whale_jit.h）、
 * 不直接链接 TinyCC，只依赖 libwhalejs.a（或 whalejs.dll + 导入库）。
 *
 * 构建（本仓库内）：xmake build whale_demo
 * 构建（分发形态）：
 *     cc embed.c -I<prefix>/include -L<prefix>/lib -lwhalejs
 * Windows 动态库还要定义 USING_QJS_SHARED，见 README.md。
 *
 * 注意脚本形状：本引擎对"全局函数自递归"（顶层 `function fib`，`fib` 是全局
 * 变量）不编译，函数体里的自引用必须是闭包变量。所以热点函数放在 IIFE 里，
 * 这也是 perftest 用例的形状。
 */
#include <stdio.h>
#include <string.h>

#include "whalejs.h"

static const char *SCRIPT =
    "(function () {\n"
    "  function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n"
    "  var c = 0;\n"
    "  for (var i = 0; i < 20; i++) c = fib(26);\n"
    "  return c;\n"
    "})();";

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx;
    JSValue v;
    JSWhaleJitStats st;
    double result = 0;

    if (!rt) {
        fprintf(stderr, "JS_NewRuntime failed\n");
        return 1;
    }
    ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "JS_NewContext failed\n");
        JS_FreeRuntime(rt);
        return 1;
    }

    v = JS_Eval(ctx, SCRIPT, strlen(SCRIPT), "embed.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        fprintf(stderr, "eval failed: %s\n", s ? s : "?");
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }
    JS_ToFloat64(ctx, &result, v);
    JS_FreeValue(ctx, v);

    JS_WhaleGetJitStats(ctx, &st);
    printf("WhaleJS %s: fib(26) x20 = %.0f\n", JS_WhaleGetVersion(), result);
    printf("  jit mode  = %d\n", (int)JS_WhaleGetJitMode(ctx));
    printf("  requests  = %u\n", st.requests);
    printf("  compiled  = %u\n", st.compiled);
    printf("  failed    = %u\n", st.failed);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return (result == 121393 && st.compiled > 0) ? 0 : 1;
}
