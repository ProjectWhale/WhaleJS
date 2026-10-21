/*
 * Promise hook coverage: BEFORE/AFTER must bracket the reaction handler for
 * BOTH `await` and `.then()`.
 *
 * `await` does not call the JS-visible `then` - it goes straight to
 * perform_promise_then - so a JS wrapper around Promise.prototype.then cannot
 * see it. Both paths end in promise_reaction_job, which is why the hook pair
 * lives there.
 *
 * Build/run:  xmake run whale_promise_hook_test   (or link like whale_test)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

/* Record the hook sequence. The JS side appends its own marker when a handler
 * runs, so the interleaving is visible: ... BEFORE h AFTER ... */
static char seq[512];

static void append(const char *s)
{
    size_t n = strlen(seq);

    if (n + strlen(s) + 1 < sizeof(seq))
        strcat(seq, s);
}

static void promise_hook(JSContext *ctx, JSPromiseHookType type,
                         JSValueConst promise, JSValueConst parent,
                         void *opaque)
{
    (void)ctx; (void)promise; (void)parent; (void)opaque;
    switch (type) {
    case JS_PROMISE_HOOK_INIT:    append("I"); break;
    case JS_PROMISE_HOOK_BEFORE:  append("B"); break;
    case JS_PROMISE_HOOK_AFTER:   append("A"); break;
    case JS_PROMISE_HOOK_RESOLVE: append("R"); break;
    }
}

static int eval_ok(JSContext *ctx, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<promise-hook>",
                        JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);

        printf("FAIL exception: %s\n", s ? s : "?");
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        failures++;
        JS_FreeValue(ctx, v);
        return 0;
    }
    JS_FreeValue(ctx, v);
    return 1;
}

int main(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global, v;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    rt = JS_NewRuntime();
    CHECK(rt != NULL);
    ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    if (!ctx)
        return 1;

    JS_SetPromiseHook(rt, promise_hook, NULL);

    /* ---- 1. .then() ---- */
    seq[0] = '\0';
    rc = eval_ok(ctx,
        "var seen = [];\n"
        "var p = Promise.resolve(1);\n"
        "var q = p.then(function (v) { seen.push('h'); return v + 1; });\n"
        "q.then(function (v) { seen.push('h2'); return v; });\n"
        "0");
    CHECK(rc);
    /* drain the job queue */
    JS_RunGC(rt);
    while (JS_IsJobPending(rt)) {
        JSContext *c;

        if (JS_ExecutePendingJob(rt, &c) < 0) {
            printf("FAIL: a job threw\n");
            failures++;
            break;
        }
    }
    printf(".then   sequence: %s\n", seq);
    /* at least two reactions must be bracketed */
    {
        int b = 0, a = 0;
        const char *s;

        for (s = seq; *s; s++) {
            if (*s == 'B') b++;
            if (*s == 'A') a++;
        }
        printf("  BEFORE=%d AFTER=%d\n", b, a);
        CHECK(b >= 2);
        CHECK(a >= 2);
        CHECK(b == a);          /* every BEFORE got its AFTER */
    }

    /* ---- 2. await ---- */
    seq[0] = '\0';
    rc = eval_ok(ctx,
        "var log = [];\n"
        "(async function () {\n"
        "  log.push('start');\n"
        "  await null;\n"
        "  log.push('after-await');\n"
        "  await Promise.resolve(2);\n"
        "  log.push('after-await2');\n"
        "})();\n"
        "0");
    CHECK(rc);
    while (JS_IsJobPending(rt)) {
        JSContext *c;

        if (JS_ExecutePendingJob(rt, &c) < 0) {
            printf("FAIL: a job threw\n");
            failures++;
            break;
        }
    }
    printf("await   sequence: %s\n", seq);
    {
        int b = 0, a = 0;
        const char *s;

        for (s = seq; *s; s++) {
            if (*s == 'B') b++;
            if (*s == 'A') a++;
        }
        printf("  BEFORE=%d AFTER=%d\n", b, a);
        /* two awaits -> at least two reactions */
        CHECK(b >= 2);
        CHECK(a >= 2);
        CHECK(b == a);
    }

    /* ---- 3. the JS side really sees a balanced stack ---- */
    {
        int depth = 0, maxdepth = 0, i;

        for (i = 0; seq[i]; i++) {
            if (seq[i] == 'B')
                depth++;
            else if (seq[i] == 'A')
                depth--;
            if (depth > maxdepth)
                maxdepth = depth;
            CHECK(depth >= 0);      /* never more AFTERs than BEFOREs */
        }
        CHECK(depth == 0);          /* balanced at the end */
        printf("  max nesting=%d, final depth=%d\n", maxdepth, depth);
    }

    /* ---- 4. rejection path also brackets ---- */
    seq[0] = '\0';
    rc = eval_ok(ctx,
        "Promise.reject(new Error('x')).catch(function (e) { return 1; });\n"
        "0");
    CHECK(rc);
    while (JS_IsJobPending(rt)) {
        JSContext *c;

        if (JS_ExecutePendingJob(rt, &c) < 0) {
            printf("FAIL: a job threw\n");
            failures++;
            break;
        }
    }
    printf("reject  sequence: %s\n", seq);
    {
        int b = 0, a = 0;
        const char *s;

        for (s = seq; *s; s++) {
            if (*s == 'B') b++;
            if (*s == 'A') a++;
        }
        CHECK(b >= 1);
        CHECK(b == a);
    }

    /* ---- 5. with no hook installed, promises still work ---- */
    JS_SetPromiseHook(rt, NULL, NULL);
    rc = eval_ok(ctx,
        "var ok = 0;\n"
        "Promise.resolve(41).then(function (v) { ok = v + 1; });\n"
        "0");
    CHECK(rc);
    while (JS_IsJobPending(rt)) {
        JSContext *c;

        if (JS_ExecutePendingJob(rt, &c) < 0) {
            printf("FAIL: a job threw\n");
            failures++;
            break;
        }
    }
    v = JS_Eval(ctx, "ok", 2, "<x>", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_ToInt32(ctx, (int32_t *)&rc, v) == 0 && rc == 42);
    JS_FreeValue(ctx, v);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (failures) {
        printf("promise hook test: %d FAILED\n", failures);
        return 1;
    }
    printf("promise hook test: ok\n");
    return 0;
}
