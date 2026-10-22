/*
 * WebAssembly extension test.
 *
 * Build and run:  xmake run whale_wasm_test
 *
 * The module bytes below are assembled by hand, so the test needs no fixture
 * file and no wat2wasm. Each module's shape is described above its array.
 *
 * Two paths are exercised throughout: the translator (wasm -> C -> the
 * engine's TinyCC) for the functions it can handle, and wasm3 for the rest.
 * A function the translator declines is not a failure - it just runs slower -
 * so the checks are on values, not on which path produced them.
 */
#include <stdio.h>
#include <stdlib.h>
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

/*
 * (module
 *   (import "env" "log"    (func (param i32)))
 *   (import "env" "zero"   (func (result i32)))
 *   (import "env" "addtwo" (func (param i32 i32) (result i32)))
 *   (memory (export "memory") 1 2)
 *   (func (export "add")     (param i32 i32) (result i32))   i32.add
 *   (func (export "poke")    (param i32 i32))                i32.store8
 *   (func (export "peek")    (param i32) (result i32))       i32.load8_u
 *   (func (export "grow")    (param i32) (result i32))       memory.grow
 *   (func (export "callimp") (param i32) (result i32))       call log; +100
 *   (func (export "dbl")     (param f64) (result f64))       f64.add
 *   (func (export "inc64")   (param i64) (result i64))       i64.add 1
 *   (func (export "impzero") (result i32))                   call zero
 *   (func (export "impadd")  (param i32 i32) (result i32))   call addtwo
 * )
 */
static const unsigned char MODULE[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x23, 0x07, 0x60,
    0x01, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60,
    0x01, 0x7c, 0x01, 0x7c, 0x60, 0x01, 0x7e, 0x01, 0x7e, 0x02, 0x23, 0x03,
    0x03, 0x65, 0x6e, 0x76, 0x03, 0x6c, 0x6f, 0x67, 0x00, 0x00, 0x03, 0x65,
    0x6e, 0x76, 0x04, 0x7a, 0x65, 0x72, 0x6f, 0x00, 0x01, 0x03, 0x65, 0x6e,
    0x76, 0x06, 0x61, 0x64, 0x64, 0x74, 0x77, 0x6f, 0x00, 0x02, 0x03, 0x0a,
    0x09, 0x02, 0x03, 0x04, 0x04, 0x04, 0x05, 0x06, 0x01, 0x02, 0x05, 0x04,
    0x01, 0x01, 0x01, 0x02, 0x07, 0x50, 0x0a, 0x03, 0x61, 0x64, 0x64, 0x00,
    0x03, 0x04, 0x70, 0x6f, 0x6b, 0x65, 0x00, 0x04, 0x04, 0x70, 0x65, 0x65,
    0x6b, 0x00, 0x05, 0x04, 0x67, 0x72, 0x6f, 0x77, 0x00, 0x06, 0x07, 0x63,
    0x61, 0x6c, 0x6c, 0x69, 0x6d, 0x70, 0x00, 0x07, 0x03, 0x64, 0x62, 0x6c,
    0x00, 0x08, 0x05, 0x69, 0x6e, 0x63, 0x36, 0x34, 0x00, 0x09, 0x07, 0x69,
    0x6d, 0x70, 0x7a, 0x65, 0x72, 0x6f, 0x00, 0x0a, 0x06, 0x69, 0x6d, 0x70,
    0x61, 0x64, 0x64, 0x00, 0x0b, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79,
    0x02, 0x00, 0x0a, 0x4d, 0x09, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a,
    0x0b, 0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0x36, 0x00, 0x00, 0x0b, 0x07,
    0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b, 0x06, 0x00, 0x20, 0x00, 0x40,
    0x00, 0x0b, 0x0c, 0x00, 0x20, 0x00, 0x10, 0x00, 0x20, 0x00, 0x41, 0xe4,
    0x00, 0x6a, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x20, 0x00, 0xa0, 0x0b, 0x07,
    0x00, 0x20, 0x00, 0x42, 0x01, 0x7c, 0x0b, 0x04, 0x00, 0x10, 0x01, 0x0b,
    0x08, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x02, 0x0b,
};

/* (module (import "env" "fn" (func (result i32)))
 *         (func (export "g") (result i32) call fn)) */
static const unsigned char THROW_MODULE[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x02, 0x0a, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x02, 0x66, 0x6e, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x05, 0x01, 0x01, 0x67, 0x00, 0x01,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0b,
};

/*
 * Cross-function calls, globals, and signatures wider than two parameters.
 *
 * (module
 *   (memory (export "memory") 1)
 *   (global $acc (mut i32) (i32.const 5))
 *   (global $k   (mut f64) (f64.const 1.5))
 *   (func $outer (param i32 i32) (result i32)      ; calls $inner twice,
 *         (i32.add (call $inner ..) (call $inner ..)))  ; which is declared later
 *   (func $inner (param i32 i32) (result i32) (i32.mul (i32.add ..) 2))
 *   (func (export "sum3")  (param i32 i32 i32) (result i32))
 *   (func (export "sum4")  (param i32 i32 i32 i32) (result i32))
 *   (func (export "mix")   (param i32 f64 i64) (result f64))
 *   (func (export "bump")  (param i32) (result i32))   acc += n
 *   (func (export "getk")  (result f64))
 *   (func (export "setk")  (param f64) (result f64))
 *   (func (export "zero")  (result i32))               no params, calls $inner
 *   (func (export "pages") (result i32))               memory.size
 *   (func (export "outer") (param i32 i32) (result i32))
 * )
 *
 * $outer calls $inner, which is defined *after* it: the translator refuses a
 * call to a function it has not compiled yet, so this module only translates
 * fully if the binding iterates to a fixpoint.
 */
static const unsigned char XFUNC_MODULE[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x2f, 0x08, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x03, 0x7f, 0x7c,
    0x7e, 0x01, 0x7c, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7c,
    0x60, 0x01, 0x7c, 0x01, 0x7c, 0x60, 0x00, 0x01, 0x7f, 0x03, 0x0d, 0x0c,
    0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x07, 0x00, 0x01,
    0x05, 0x03, 0x01, 0x00, 0x01, 0x06, 0x12, 0x02, 0x7f, 0x01, 0x41, 0x05,
    0x0b, 0x7c, 0x01, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f,
    0x0b, 0x07, 0x50, 0x0b, 0x04, 0x73, 0x75, 0x6d, 0x33, 0x00, 0x02, 0x04,
    0x73, 0x75, 0x6d, 0x34, 0x00, 0x03, 0x03, 0x6d, 0x69, 0x78, 0x00, 0x04,
    0x04, 0x62, 0x75, 0x6d, 0x70, 0x00, 0x05, 0x04, 0x67, 0x65, 0x74, 0x6b,
    0x00, 0x06, 0x04, 0x73, 0x65, 0x74, 0x6b, 0x00, 0x07, 0x04, 0x7a, 0x65,
    0x72, 0x6f, 0x00, 0x08, 0x05, 0x70, 0x61, 0x67, 0x65, 0x73, 0x00, 0x09,
    0x05, 0x6f, 0x75, 0x74, 0x65, 0x72, 0x00, 0x0a, 0x03, 0x73, 0x65, 0x6c,
    0x00, 0x0b, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0a,
    0x81, 0x01, 0x0c, 0x0f, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x01, 0x20,
    0x01, 0x20, 0x00, 0x10, 0x01, 0x6a, 0x0b, 0x0a, 0x00, 0x20, 0x00, 0x20,
    0x01, 0x6a, 0x41, 0x02, 0x6c, 0x0b, 0x0a, 0x00, 0x20, 0x00, 0x20, 0x01,
    0x6a, 0x20, 0x02, 0x6a, 0x0b, 0x0d, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a,
    0x20, 0x02, 0x6a, 0x20, 0x03, 0x6a, 0x0b, 0x0c, 0x00, 0x20, 0x00, 0xb7,
    0x20, 0x01, 0x20, 0x02, 0xb9, 0xa0, 0xa0, 0x0b, 0x0b, 0x00, 0x23, 0x00,
    0x20, 0x00, 0x6a, 0x24, 0x00, 0x23, 0x00, 0x0b, 0x04, 0x00, 0x23, 0x01,
    0x0b, 0x08, 0x00, 0x20, 0x00, 0x24, 0x01, 0x23, 0x01, 0x0b, 0x08, 0x00,
    0x41, 0x03, 0x41, 0x04, 0x10, 0x01, 0x0b, 0x04, 0x00, 0x3f, 0x00, 0x0b,
    0x08, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x00, 0x0b, 0x0d, 0x00, 0x41,
    0xe3, 0x00, 0x1a, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x1b, 0x0b,
};

/* Translator regressions found with vite's embedded oxc module.
 *
 * (module
 *   ;; br_if 0 to a block that carries a value: the condition is popped, the
 *   ;; value stays on the stack for the fallthrough.
 *   (func (export "count_block") (param i32) (result i32)
 *     block (result i32)
 *       i32.const 7
 *       local.get 0
 *       br_if 0
 *       drop
 *       i32.const 9
 *     end)
 *   ;; two functions with loops in one module: their C labels must not collide
 *   (func (export "loop_a") (param i32 i32) (result i32) ... sum 0..n)
 *   (func (export "loop_b") (param i32 i32) (result i32) ... sum 3*i)
 *   ;; br 1 targets the function block, which has no label - it must be a return
 *   (func (export "br_to_body") (param i32) (result i32)
 *     local.get 0
 *     if
 *       i32.const 5
 *       br 1
 *     end
 *     i32.const 6)
 *   ;; a block whose end lands right before the function's `}`
 *   (func (export "label_at_end") (result i32) block end i32.const 11)
 * )
 */
static const unsigned char REGRESS_MODULE[] = {
0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x03, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00,
    0x01, 0x7f, 0x03, 0x06, 0x05, 0x00, 0x01, 0x01, 0x00, 0x02, 0x07, 0x3d,
    0x05, 0x0b, 0x63, 0x6f, 0x75, 0x6e, 0x74, 0x5f, 0x62, 0x6c, 0x6f, 0x63,
    0x6b, 0x00, 0x00, 0x06, 0x6c, 0x6f, 0x6f, 0x70, 0x5f, 0x61, 0x00, 0x01,
    0x06, 0x6c, 0x6f, 0x6f, 0x70, 0x5f, 0x62, 0x00, 0x02, 0x0a, 0x62, 0x72,
    0x5f, 0x74, 0x6f, 0x5f, 0x62, 0x6f, 0x64, 0x79, 0x00, 0x03, 0x0c, 0x6c,
    0x61, 0x62, 0x65, 0x6c, 0x5f, 0x61, 0x74, 0x5f, 0x65, 0x6e, 0x64, 0x00,
    0x04, 0x0a, 0x71, 0x05, 0x0e, 0x00, 0x02, 0x7f, 0x41, 0x07, 0x20, 0x00,
    0x0d, 0x00, 0x1a, 0x41, 0x09, 0x0b, 0x0b, 0x23, 0x01, 0x02, 0x7f, 0x02,
    0x40, 0x03, 0x40, 0x20, 0x01, 0x20, 0x00, 0x4e, 0x0d, 0x01, 0x20, 0x02,
    0x20, 0x01, 0x6a, 0x21, 0x02, 0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x01,
    0x0c, 0x00, 0x0b, 0x0b, 0x20, 0x02, 0x0b, 0x26, 0x01, 0x02, 0x7f, 0x02,
    0x40, 0x03, 0x40, 0x20, 0x01, 0x20, 0x00, 0x4e, 0x0d, 0x01, 0x20, 0x02,
    0x20, 0x01, 0x41, 0x03, 0x6c, 0x6a, 0x21, 0x02, 0x20, 0x01, 0x41, 0x01,
    0x6a, 0x21, 0x01, 0x0c, 0x00, 0x0b, 0x0b, 0x20, 0x02, 0x0b, 0x0d, 0x00,
    0x20, 0x00, 0x04, 0x40, 0x41, 0x05, 0x0c, 0x01, 0x0b, 0x41, 0x06, 0x0b,
    0x07, 0x00, 0x02, 0x40, 0x0b, 0x41, 0x0b, 0x0b,
};

/* (module
 *   (func (export "spin")   (param i32) (result i32) (local i32)
 *         loop { local.set 1 (+ 1); br 0 })       - never returns
 *   (func (export "spin_n") (param i32) (result i32) (local i32))
 *   (func (export "add")    (param i32 i32) (result i32)) )
 */
static const unsigned char SPIN_MODULE[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0c, 0x02, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x03, 0x04,
    0x03, 0x00, 0x00, 0x01, 0x07, 0x17, 0x03, 0x04, 0x73, 0x70, 0x69, 0x6e,
    0x00, 0x00, 0x06, 0x73, 0x70, 0x69, 0x6e, 0x5f, 0x6e, 0x00, 0x01, 0x03,
    0x61, 0x64, 0x64, 0x00, 0x02, 0x0a, 0x39, 0x03, 0x12, 0x01, 0x01, 0x7f,
    0x03, 0x40, 0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x01, 0x0c, 0x00, 0x0b,
    0x20, 0x01, 0x0b, 0x1c, 0x01, 0x01, 0x7f, 0x02, 0x40, 0x03, 0x40, 0x20,
    0x01, 0x20, 0x00, 0x4e, 0x0d, 0x01, 0x20, 0x01, 0x41, 0x01, 0x6a, 0x21,
    0x01, 0x0c, 0x00, 0x0b, 0x0b, 0x20, 0x01, 0x0b, 0x07, 0x00, 0x20, 0x00,
    0x20, 0x01, 0x6a, 0x0b,
};

/* The interrupt handler: stop after this many polls. A module that never exits
   has to be stoppable, and the count is what proves the poll actually ran
   instead of the loop happening to finish. */
static int interrupt_polls;
static int interrupt_budget;

static int on_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt; (void)opaque;
    interrupt_polls++;
    return interrupt_budget > 0 && interrupt_polls >= interrupt_budget;
}

static void eval_and_check_string(JSContext *ctx, const char *src,
                                  const char *expected, const char *what)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<wasm-test>", JS_EVAL_TYPE_GLOBAL);
    const char *s;

    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        s = JS_ToCString(ctx, e);
        printf("FAIL %s: exception %s\n", what, s ? s : "?");
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        failures++;
        JS_FreeValue(ctx, v);
        return;
    }
    s = JS_ToCString(ctx, v);
    if (!s || strcmp(s, expected) != 0) {
        printf("FAIL %s: got %s, expected %s\n", what, s ? s : "(null)", expected);
        failures++;
    }
    if (s)
        JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
}

int main(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue v, global, mod_ab, bad_ab, throw_ab;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    rt = JS_NewRuntime();
    CHECK(rt != NULL);
    if (!rt)
        return 1;
    ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 1;
    }

    /* installing more than once must be harmless */
    CHECK(JS_WhaleInstallWasm(ctx) == 0);
    CHECK(JS_WhaleInstallWasmRuntime(rt) == 0);
    CHECK(JS_WhaleInstallWasm(ctx) == 0);

    global = JS_GetGlobalObject(ctx);
    mod_ab = JS_NewArrayBufferCopy(ctx, MODULE, sizeof(MODULE));
    bad_ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)"nope", 4);
    throw_ab = JS_NewArrayBufferCopy(ctx, THROW_MODULE, sizeof(THROW_MODULE));
    JS_SetPropertyStr(ctx, global, "__mod", mod_ab);
    JS_SetPropertyStr(ctx, global, "__bad", bad_ab);
    JS_SetPropertyStr(ctx, global, "__throwmod", throw_ab);
    JS_FreeValue(ctx, global);

    eval_and_check_string(ctx,
        "WebAssembly.validate(__mod) + ',' + WebAssembly.validate(__bad)",
        "true,false", "validate");

    /* Every export, through whatever path it took. */
    eval_and_check_string(ctx,
        "var seen = -1;\n"
        "var imports = { env: {\n"
        "  log: function (v) { seen = v; },\n"
        "  zero: function () { return 1234; },\n"
        "  addtwo: function (a, b) { return a + b; }\n"
        "} };\n"
        "var mod = WebAssembly.compile(__mod);\n"
        "var inst = WebAssembly.instantiate(mod, imports);\n"
        "var e = inst.exports;\n"
        "var r = [];\n"
        "r.push(e.add(40, 2));\n"
        "e.poke(16, 0x44);\n"
        "var m = new Uint8Array(e.memory.buffer);\n"
        "r.push(m[16].toString(16));\n"
        "r.push(e.peek(16));\n"
        "r.push(e.dbl(21.5));\n"
        "r.push(e.callimp(7));\n"
        "r.push(seen);\n"
        "r.push(String(e.inc64(9007199254740993n)));\n"
        "r.push(e.impzero());\n"
        "r.push(e.impadd(20, 22));\n"
        "r.push(e.grow(1));\n"
        "r.push(new Uint8Array(e.memory.buffer).length);\n"
        "JSON.stringify(r)",
        "[42,\"44\",68,43,107,7,\"9007199254740994\",1234,42,1,131072]",
        "exports");

    /* the same Module can be instantiated again, independently. The second
       instantiate relocates the engine's TCC state, which moves every function
       the first one had already bound - so this also checks that the first
       instance still works afterwards. */
    eval_and_check_string(ctx,
        "var inst2 = WebAssembly.instantiate(mod, imports);\n"
        "inst2.exports.add(1, 2) + ':' + inst.exports.add(3, 4)",
        "3:7", "re-instantiate");

    /* instantiate(bytes) returns { module, instance } */
    eval_and_check_string(ctx,
        "var pair = WebAssembly.instantiate(__mod, imports);\n"
        "pair.instance.exports.add(20, 22)",
        "42", "instantiate(bytes)");

    /* an import that throws surfaces as a JS exception, not a trap string */
    eval_and_check_string(ctx,
        "var threw = 0;\n"
        "try {\n"
        "  var ti = WebAssembly.instantiate(__throwmod, {\n"
        "    env: { fn: function () { throw new Error('from import'); } }\n"
        "  }).instance;\n"
        "  ti.exports.g();\n"
        "} catch (err) { threw = String(err); }\n"
        "threw",
        "Error: from import", "import exception");

    /* a bad module must be rejected by compile() */
    eval_and_check_string(ctx,
        "var ok = 0;\n"
        "try { WebAssembly.compile(__bad); } catch (e) { ok = 1; }\n"
        "ok",
        "1", "bad module rejected");

    /* memory.grow detaches the old buffer and hands out a new one */
    eval_and_check_string(ctx,
        "var g = WebAssembly.instantiate(__mod, imports).instance;\n"
        "var b0 = g.exports.memory.buffer;\n"
        "var n0 = b0.byteLength;\n"
        "g.exports.grow(1);\n"
        "var b1 = g.exports.memory.buffer;\n"
        "(b0 !== b1) + ':' + b0.byteLength + ':' + n0 + ':' + b1.byteLength",
        "true:0:65536:131072", "grow detaches");

    /* ---- cross-function calls, globals, wide signatures ---- */
    {
        JSValue g3, xab;

        g3 = JS_GetGlobalObject(ctx);
        xab = JS_NewArrayBufferCopy(ctx, XFUNC_MODULE, sizeof(XFUNC_MODULE));
        JS_SetPropertyStr(ctx, g3, "__xfunc", xab);
        JS_FreeValue(ctx, g3);

        /* $outer calls $inner, defined after it: this only translates if the
           binding iterates to a fixpoint. $inner(a,b) is (a+b)*2, so
           outer(1,2) = 6 + 6. */
        eval_and_check_string(ctx,
            "var xi = WebAssembly.instantiate(__xfunc, {}).instance;\n"
            "xi.exports.outer(1, 2)",
            "12", "call to a later sibling");

        /* Signatures the old hand-written dispatcher did not cover. */
        eval_and_check_string(ctx,
            "xi.exports.sum3(1, 2, 3) + ':' + xi.exports.sum4(1, 2, 3, 4)",
            "6:10", "3 and 4 parameters");

        /* A mixed signature in both directions: i32 -> f64 and i64 -> f64. */
        eval_and_check_string(ctx,
            "xi.exports.mix(2, 3.5, 4n)",
            "9.5", "mixed parameter types");

        /* Globals: read-modify-write across calls, so the shadow and wasm3's
           copy have to stay in step. acc starts at 5. */
        eval_and_check_string(ctx,
            "xi.exports.bump(10) + ':' + xi.exports.bump(1)",
            "15:16", "i32 global");

        eval_and_check_string(ctx,
            "xi.exports.getk() + ':' + xi.exports.setk(2.25) + ':' + xi.exports.getk()",
            "1.5:2.25:2.25", "f64 global");

        /* No parameters at all, with a call in the body. */
        eval_and_check_string(ctx,
            "xi.exports.zero() + ':' + xi.exports.pages()",
            "14:1", "zero-parameter function");

        /* drop and select, both of which clang emits constantly */
        eval_and_check_string(ctx,
            "xi.exports.sel(11, 22, 1) + ':' + xi.exports.sel(11, 22, 0)",
            "11:22", "drop and select");
    }

    /* ---- translator regressions (vite's embedded oxc module) ---- */
    {
        JSValue g4, rab;

        g4 = JS_GetGlobalObject(ctx);
        rab = JS_NewArrayBufferCopy(ctx, REGRESS_MODULE, sizeof(REGRESS_MODULE));
        JS_SetPropertyStr(ctx, g4, "__regress", rab);
        JS_FreeValue(ctx, g4);

        /* br_if 0 to a value-carrying block: taken -> 7, not taken -> 9 */
        eval_and_check_string(ctx,
            "var rg = WebAssembly.instantiate(__regress, {}).instance;\n"
            "rg.exports.count_block(1) + ':' + rg.exports.count_block(0)",
            "7:9", "br_if to a value-carrying block");

        /* two loops in one module: the second one's labels must not collide */
        eval_and_check_string(ctx,
            "rg.exports.loop_a(10, 0) + ':' + rg.exports.loop_b(10, 0)",
            "45:135", "two loops in one module");

        /* br to the function block becomes a return */
        eval_and_check_string(ctx,
            "rg.exports.br_to_body(1) + ':' + rg.exports.br_to_body(0)",
            "5:6", "br to the function block");

        /* a label that lands right before the closing brace */
        eval_and_check_string(ctx,
            "rg.exports.label_at_end()",
            "11", "label at end of block");
    }

    /* ---- interrupt: a wasm loop that never exits must be stoppable ---- */
    {
        JSValue spin_ab, g2;
        JSValue r;
        int32_t n;

        g2 = JS_GetGlobalObject(ctx);
        spin_ab = JS_NewArrayBufferCopy(ctx, SPIN_MODULE, sizeof(SPIN_MODULE));
        JS_SetPropertyStr(ctx, g2, "__spin", spin_ab);
        JS_FreeValue(ctx, g2);

        /* First: a finite loop under the same metering still returns the right
           answer. That is the control for the test below - if metering or the
           suspend/resume path broke ordinary execution, this would show it. */
        eval_and_check_string(ctx,
            "var si = WebAssembly.instantiate(__spin, {}).instance;\n"
            "si.exports.spin_n(100000) + ':' + si.exports.add(20, 22)",
            "100000:42", "metered loop still runs");

        /* Now the infinite one. The handler asks to stop after a few polls;
           without the poll the call never comes back at all. The error is
           deliberately uncatchable - the same as the engine's own interrupt -
           so it comes out of JS_Eval rather than into a JS catch block. */
        interrupt_polls = 0;
        interrupt_budget = 3;
        JS_SetInterruptHandler(rt, on_interrupt, NULL);

        r = JS_Eval(ctx,
            "var si2 = WebAssembly.instantiate(__spin, {}).instance;\n"
            "si2.exports.spin(1)",
            strlen("var si2 = WebAssembly.instantiate(__spin, {}).instance;\n"
                   "si2.exports.spin(1)"),
            "<wasm-test>", JS_EVAL_TYPE_GLOBAL);
        JS_SetInterruptHandler(rt, NULL, NULL);

        printf("interrupt: polls=%d (expected >=3)\n", interrupt_polls);
        CHECK(interrupt_polls >= 3);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, e);
            const char *msg = s ? s : "";

            printf("  raised: %s\n", msg);
            CHECK(strstr(msg, "interrupted") != NULL);
            if (s)
                JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, e);
        } else {
            printf("FAIL interrupt: the infinite loop returned\n");
            failures++;
        }
        JS_FreeValue(ctx, r);

        /* The stopped instance refuses further calls rather than resuming a
           half-executed frame; a fresh instantiation is unaffected. */
        eval_and_check_string(ctx,
            "var blocked = 0;\n"
            "try { si2.exports.spin_n(10); } catch (e) { blocked = 1; }\n"
            "blocked + ':' + WebAssembly.instantiate(__spin, {}).instance.exports.spin_n(10)",
            "1:10", "interrupted instance retired");
        (void)n;
    }

    /* drop everything: the finalizers have to release the wasm runtimes */
    v = JS_Eval(ctx,
                "mod = inst = inst2 = pair = g = e = m = si = si2 = null; 'ok'",
                strlen("mod = inst = inst2 = pair = g = e = m = si = si2 = null; 'ok'"),
                "<wasm-test>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, v);
    JS_RunGC(rt);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (failures) {
        printf("whale wasm test: %d FAILED\n", failures);
        return 1;
    }
    printf("whale wasm test: ok\n");
    return 0;
}

