/*
 * WebAssembly extension for WhaleJS, built on wasm3.
 *
 * This file is the whole binding. It lives in a separate static library
 * (xmake target WhaleJS-wasm) and uses only the public headers: quickjs.h,
 * whalejs.h and wasm3.h. Nothing in the engine changes, and a build that does
 * not link this library behaves exactly as before.
 *
 * Design notes
 *
 * - A Module object is just a validated copy of the bytes. instantiate()
 *   re-parses them into a fresh wasm3 environment, so one Module can be
 *   instantiated any number of times (wasm3 itself allows a parsed module to
 *   be loaded into one runtime only).
 *
 * - Instance, the exported functions and the memory view all share one
 *   reference-counted WasmCore. They can outlive each other in any order
 *   without dangling, and no JS refcount cycle is created (the core is not a
 *   JSValue).
 *
 * - Imports are linked through one generic trampoline: wasm3 hands the
 *   IM3Function to it, so the argument types are read at call time instead of
 *   needing one C function per signature.
 *
 * - The memory view is zero-copy: an ArrayBuffer over wasm3's linear memory.
 *   memory.grow reallocates that block, so the old ArrayBuffer is detached and
 *   a new one handed out on the next access. Detaching is checked after every
 *   exported call, so the window is one call, not one GC.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "whalejs.h"
#include "wasm3.h"
/* Module internals: the globals live in M3Module, and the shadow array the
   translated code indexes has to be filled from them. wasm3.h deliberately
   exposes only the function-level API. */
#include "m3_env.h"

/* The translator in whale_wasm_c.c. Returns 1 when the body was emitted into
   the engine's TCC state; the address only exists after the batch relocate.
   `mod_ctx` is the translator's own per-module table (opaque here), and
   `func_index` is the function's index in the module's function space. */
extern int JS_WhaleWasmTranslate(const char *name, const uint8_t *body,
                                 size_t body_len, int nparams,
                                 const int *params, int nresults,
                                 const int *results,
                                 void *mod_ctx, int func_index);
/* The translator's module table: created by the binding, filled with every
   function's signature, and consulted for `call`. */
extern void *JS_WhaleWasmModuleNew(int nfuncs);
extern void JS_WhaleWasmModuleFree(void *mod);
extern void JS_WhaleWasmModuleSetFunc(void *mod, int index, const int *params,
                                      int nparams, const int *results,
                                      int nresults, const char *sym);
extern void JS_WhaleWasmModuleSetGlobal(void *mod, int index, int type);
extern void JS_WhaleWasmModuleMarkTranslated(void *mod, int index);
extern int whale_wasm_trapped(void);
extern void whale_wasm_clear_trap(void);
extern int whale_div_trapped(void);
extern int whale_wasm_interrupted(void);
extern void whale_wasm_set_interrupt_probe(int (*probe)(void *), void *opaque);

/* Diagnostic switch, looked up once. Not a configuration knob: it only turns on
 * the per-call tracing used while working on the translator. */
static int whale_wasm_dbg(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("WHALE_WASM_DBG") != NULL;
    return on;
}

/* The probe translated loops call. It has to be a plain C function with no
 * arguments, so the JSContext travels in the opaque slot. */
static int wasm_interrupt_probe(void *opaque)
{
    return JS_WhalePollInterrupt((JSContext *)opaque);
}

/* ------------------------------------------------------------------ */
/* class ids, per runtime                                              */
/* ------------------------------------------------------------------ */
typedef struct WasmClasses {
    JSRuntime *rt;
    JSClassID module;
    JSClassID instance;
    JSClassID memory;
    JSClassID global;
} WasmClasses;

#define WASM_MAX_RUNTIMES 16
static WasmClasses wasm_classes_tab[WASM_MAX_RUNTIMES];

static WasmClasses *wasm_classes_find(JSRuntime *rt)
{
    int i;

    for (i = 0; i < WASM_MAX_RUNTIMES; i++) {
        if (wasm_classes_tab[i].rt == rt)
            return &wasm_classes_tab[i];
    }
    return NULL;
}

static WasmClasses *wasm_classes_get(JSRuntime *rt)
{
    WasmClasses *c = wasm_classes_find(rt);
    int i;

    if (c) {
        /* The table is keyed by pointer, and a freed runtime's address can be
           handed to a new one. A class id from the old runtime is not
           registered in the new one's class array, so that is the test for
           whether the entry is stale. */
        if (JS_IsRegisteredClass(rt, c->module) &&
            JS_IsRegisteredClass(rt, c->instance) &&
            JS_IsRegisteredClass(rt, c->memory) &&
            JS_IsRegisteredClass(rt, c->global))
            return c;
        c->module = c->instance = c->memory = c->global = 0;
    }
    if (!c) {
        for (i = 0; i < WASM_MAX_RUNTIMES; i++) {
            if (!wasm_classes_tab[i].rt) {
                c = &wasm_classes_tab[i];
                c->rt = rt;
                break;
            }
        }
    }
    if (!c)
        return NULL;
    JS_NewClassID(rt, &c->module);
    JS_NewClassID(rt, &c->instance);
    JS_NewClassID(rt, &c->memory);
    JS_NewClassID(rt, &c->global);
    return c;
}

/* ------------------------------------------------------------------ */
/* the shared core                                                     */
/* ------------------------------------------------------------------ */
typedef struct WasmCore WasmCore;

/* One translated function: a native entry plus the wasm signature it was
   compiled for. The table is dense over the module's function index space, so
   an export that names function `k` finds tab[k]; `entry == NULL` means the
   translator declined and that function keeps running on wasm3. */
typedef struct NativeFn {
    void *entry;
    void *wentry;           /* the uniform-ABI wrapper; what the binding calls */
    int nparams;
    int params[32];
    int nresults;
    int results[4];
    char sym[40];           /* the symbol to re-resolve after a relocate */
    char wsym[48];          /* the uniform entry wrapper, re-resolved too */
} NativeFn;

typedef struct WasmImportLink {
    JSValue fn;
    WasmCore *core;
    struct WasmImportLink *next;
} WasmImportLink;

struct WasmCore {
    int refs;
    JSRuntime *jrt;
    JSContext *ctx;         /* borrowed: only used from JS calls */
    IM3Environment env;
    IM3Runtime rt;
    IM3Module mod;
    JSValue bytes;          /* wasm3 points into these; keep them alive */
    WasmImportLink *imports;
    int pending_exc;        /* a JS exception escaped an import */
    int poisoned;           /* an interrupt abandoned a call mid-execution */
    NativeFn *native;       /* dense over the function index space, or NULL */
    int native_count;
    /* A shadow of wasm3's globals. Translated code indexes this array; it is
       synced to and from wasm3 at every call boundary, so a function that still
       runs on wasm3 sees the same values. */
    unsigned long long *globals;
    int nglobals;
    /* The relocate generation `native[]` was resolved in. Addresses from
       JS_WhaleFindSymbol only survive until the next relocate, so this is
       compared against JS_WhaleJitGeneration() before the table is used. */
    unsigned sym_gen;
    /* cached zero-copy view over linear memory */
    uint8_t *mem_ptr;
    size_t mem_size;
    JSValue mem_abuf;
    JSValue mem_view;
};

/* How much gas one m3_Call may burn before the runtime pauses. Gas is what
   gives the embedder a way to interrupt: wasm3 only yields at control-flow
   boundaries, and a paused runtime is where JS_WhalePollInterrupt can run. The
   unit is a ten-thousandth of an ewasm gas, so this is a few hundred thousand
   instructions per slice - fine-grained enough to stop promptly, coarse enough
   that the metering overhead stays small. */
#define WASM_GAS_SLICE 1000000

typedef struct WasmModule {
    JSRuntime *jrt;
    JSValue bytes;          /* ArrayBuffer, owned */
} WasmModule;

typedef struct WasmFn {
    JSRuntime *jrt;
    WasmCore *core;
    IM3Function fn;
    int native_index;       /* into core->native, or -1 for the wasm3 path */
    int nparams;
    int params[32];
    int nresults;
    int results[4];
} WasmFn;

typedef struct WasmMemory {
    JSRuntime *jrt;
    WasmCore *core;
} WasmMemory;

static void wasm_core_release(WasmCore *core);

/* Re-resolve this instance's translated entries when the shared TCCState has
 * been relocated since they were taken.
 *
 * There used to be a process-wide list of every live instance here, walked
 * after each relocate to rebind them all. It crashed as soon as a JSRuntime was
 * destroyed: the list held WasmCore pointers owned by that runtime, and a later
 * relocate walked them as freed memory (reproduced with two runtimes and no
 * translated code at all - the list head itself was dangling). Ownership cannot
 * cross the runtime boundary like that.
 *
 * A generation counter does the same job with no cross-runtime pointers: each
 * instance compares the counter it resolved against, and re-resolves its own
 * table lazily, on the next call. Nothing global is dereferenced. */
static void wasm_core_refresh_symbols(WasmCore *core)
{
    unsigned gen = JS_WhaleJitGeneration();
    int i;

    if (core->sym_gen == gen)
        return;
    core->sym_gen = gen;
    if (!core->native)
        return;
    for (i = 0; i < core->native_count; i++) {
        if (core->native[i].sym[0])
            core->native[i].entry = JS_WhaleFindSymbol(core->native[i].sym);
        if (core->native[i].wsym[0])
            core->native[i].wentry = JS_WhaleFindSymbol(core->native[i].wsym);
        /* the wrapper is what the binding calls; a function whose wrapper did
           not resolve cannot be used, and leaving entry set would claim it can */
        if (!core->native[i].wentry)
            core->native[i].entry = NULL;
    }
}

static WasmCore *wasm_core_ref(WasmCore *core)
{
    core->refs++;
    return core;
}

static void wasm_core_unref(WasmCore *core)
{
    if (--core->refs == 0)
        wasm_core_release(core);
}

static void wasm_core_release(WasmCore *core)
{
    WasmImportLink *l = core->imports;

    while (l) {
        WasmImportLink *next = l->next;

        JS_FreeValueRT(core->jrt, l->fn);
        js_free_rt(core->jrt, l);
        l = next;
    }
    /* the runtime owns the module once loaded */
    if (core->rt)
        m3_FreeRuntime(core->rt);
    if (core->env)
        m3_FreeEnvironment(core->env);
    if (!JS_IsNull(core->mem_view))
        JS_FreeValueRT(core->jrt, core->mem_view);
    if (!JS_IsNull(core->mem_abuf))
        JS_FreeValueRT(core->jrt, core->mem_abuf);
    /* the translated code lives in the engine's TCC state and is never freed
       individually; only the table that points at it is ours */
    free(core->native);
    free(core->globals);
    JS_FreeValueRT(core->jrt, core->bytes);
    js_free_rt(core->jrt, core);
}

/* ------------------------------------------------------------------ */
/* finalizers                                                          */
/* ------------------------------------------------------------------ */
static void js_wasm_module_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    WasmModule *m = JS_GetAnyOpaque(val, &cid);

    if (m) {
        JS_FreeValueRT(rt, m->bytes);
        js_free_rt(rt, m);
    }
}

static void js_wasm_instance_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    WasmCore *core = JS_GetAnyOpaque(val, &cid);

    if (core)
        wasm_core_unref(core);
}

static void js_wasm_memory_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    WasmMemory *m = JS_GetAnyOpaque(val, &cid);

    if (m) {
        wasm_core_unref(m->core);
        js_free_rt(rt, m);
    }
}

static void js_wasm_fn_finalizer(void *opaque)
{
    WasmFn *wf = opaque;

    if (wf) {
        wasm_core_unref(wf->core);
        js_free_rt(wf->jrt, wf);
    }
}

/* ------------------------------------------------------------------ */
/* little-endian slot helpers                                          */
/* ------------------------------------------------------------------ */
static float wasm_slot_f32(uint64_t v)
{
    float f;
    uint32_t u = (uint32_t)v;

    memcpy(&f, &u, 4);
    return f;
}

static uint64_t wasm_slot_from_f32(float f)
{
    uint64_t v = 0;

    memcpy(&v, &f, 4);
    return v;
}

/* A value type as wasm3 reports it. Typed references carry extra bits, so
   anything outside the plain M3ValueType range is treated as a reference. */
static int wasm_type(M3ValueType t)
{
    switch (t) {
    case c_m3Type_i32: return 1;
    case c_m3Type_i64: return 2;
    case c_m3Type_f32: return 3;
    case c_m3Type_f64: return 4;
    default:           return 0;    /* none / v128 / refs */
    }
}

/* ------------------------------------------------------------------ */
/* generic import trampoline                                           */
/* ------------------------------------------------------------------ */
static const void *js_wasm_trampoline(IM3Runtime rt, IM3ImportContext ic,
                                      uint64_t *sp, void *mem)
{
    WasmImportLink *link = ic->userdata;
    IM3Function f = ic->function;
    uint32_t nargs = m3_GetArgCount(f);
    uint32_t nrets = m3_GetRetCount(f);
    JSContext *ctx;
    JSValue *argv, ret;
    uint32_t i;

    (void)rt; (void)mem;
    if (!link || !link->core || !link->core->ctx)
        return m3Err_trapAbort;
    ctx = link->core->ctx;
    if (nargs > 32)
        return m3Err_trapAbort;

    argv = js_malloc(ctx, sizeof(JSValue) * (nargs ? nargs : 1));
    if (!argv)
        return m3Err_trapAbort;

    for (i = 0; i < nargs; i++) {
        uint64_t slot = sp[nrets + i];

        switch (wasm_type(m3_GetArgType(f, i))) {
        case 1: argv[i] = JS_NewInt32(ctx, (int32_t)(uint32_t)slot); break;
        case 2: argv[i] = JS_NewBigInt64(ctx, (int64_t)slot); break;
        case 3: argv[i] = JS_NewFloat64(ctx, wasm_slot_f32(slot)); break;
        case 4: {
            double d;
            memcpy(&d, &slot, 8);
            argv[i] = JS_NewFloat64(ctx, d);
            break;
        }
        default: argv[i] = JS_NULL; break;
        }
    }

    ret = JS_Call(ctx, link->fn, JS_UNDEFINED, (int)nargs, argv);
    for (i = 0; i < nargs; i++)
        JS_FreeValue(ctx, argv[i]);
    js_free(ctx, argv);

    if (JS_IsException(ret)) {
        /* the exception stays on the context; the caller turns it back into
           the JS exception after m3_Call returns */
        link->core->pending_exc = 1;
        return m3Err_trapAbort;
    }

    for (i = 0; i < nrets; i++) {
        JSValue v = ret;

        if (nrets > 1) {
            v = JS_GetPropertyUint32(ctx, ret, i);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, ret);
                link->core->pending_exc = 1;
                return m3Err_trapAbort;
            }
        }
        switch (wasm_type(m3_GetRetType(f, i))) {
        case 1: {
            int32_t n;
            if (JS_ToInt32(ctx, &n, v) < 0) goto conv_fail_v;
            sp[i] = (uint32_t)n;
            break;
        }
        case 2: {
            int64_t n;
            if (JS_ToBigInt64(ctx, &n, v) < 0) goto conv_fail_v;
            sp[i] = (uint64_t)n;
            break;
        }
        case 3: {
            double d;
            if (JS_ToFloat64(ctx, &d, v) < 0) goto conv_fail_v;
            sp[i] = wasm_slot_from_f32((float)d);
            break;
        }
        case 4: {
            double d;
            if (JS_ToFloat64(ctx, &d, v) < 0) goto conv_fail_v;
            memcpy(&sp[i], &d, 8);
            break;
        }
        default:
            sp[i] = 0;
            break;
        }
        if (nrets > 1)
            JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, ret);
    return m3Err_none;

  conv_fail_v:
    /* v is out of scope here; the exception is already on the context */
    JS_FreeValue(ctx, ret);
    link->core->pending_exc = 1;
    return m3Err_trapAbort;
}

/* ------------------------------------------------------------------ */
/* globals shadow                                                      */
/* ------------------------------------------------------------------ */
/* Translated code keeps a module's globals in a flat slot array it can index.
 * wasm3 keeps the same values in M3Global structs, and a module may be partly
 * translated, so the two have to agree across every call: this pulls wasm3's
 * values into the shadow before a native call and pushes them back after.
 *
 * The array is allocated once, when the module is instantiated, and never
 * moves - the translated code holds the pointer as a parameter, not a global. */
static void wasm_globals_load(WasmCore *core)
{
    uint32_t i;

    if (!core->globals || !core->mod)
        return;
    for (i = 0; i < (uint32_t)core->nglobals && i < core->mod->numGlobals; i++) {
        /* An imported global that resolved to another module's export lives
           there, so that is the cell to read. */
        M3Global *g = core->mod->globals[i].resolved;

        if (!g)
            g = &core->mod->globals[i];
        core->globals[i] = (unsigned long long)g->i64Value;
    }
}

static void wasm_globals_store(WasmCore *core)
{
    uint32_t i;

    if (!core->globals || !core->mod)
        return;
    for (i = 0; i < (uint32_t)core->nglobals && i < core->mod->numGlobals; i++) {
        M3Global *g = core->mod->globals[i].resolved;

        if (!g)
            g = &core->mod->globals[i];
        g->i64Value = (int64_t)core->globals[i];
    }
}

/* Allocate the shadow and seed it from the initialised globals. Returns 0 when
   the module has none. */
static int wasm_globals_init(WasmCore *core)
{
    uint32_t n;

    if (!core->mod)
        return 0;
    n = core->mod->numGlobals;
    if (!n)
        return 0;
    core->globals = calloc(n, sizeof(*core->globals));
    if (!core->globals)
        return 0;
    core->nglobals = (int)n;
    wasm_globals_load(core);
    return 1;
}

/* ------------------------------------------------------------------ */
/* exported globals                                                    */
/* ------------------------------------------------------------------ */
/* A `WebAssembly.Global`-shaped object over one slot of the module's global
 * array. Consumers read and write `.value`; a mutable global is one cell seen
 * through both this object and the translated code's shadow.
 *
 * The index is the module's global index (imports first), which is what the
 * export section names. `type` is wasm's value type byte. */
typedef struct WasmGlobalObj {
    JSRuntime *jrt;
    WasmCore *core;
    int index;
    int type;
    int mutable_;
} WasmGlobalObj;

static void js_wasm_global_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    WasmGlobalObj *g = JS_GetAnyOpaque(val, &cid);

    if (g) {
        wasm_core_unref(g->core);
        js_free_rt(rt, g);
    }
}

static JSValue js_wasm_global_value_getter(JSContext *ctx, JSValueConst this_val)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_find(rt);
    JSClassID cid;
    WasmGlobalObj *g;

    g = JS_GetAnyOpaque(this_val, &cid);
    if (!cls || !g || cid != cls->global)
        return JS_ThrowTypeError(ctx, "not a WebAssembly.Global");
    if (!g->core->globals || g->index < 0 || g->index >= g->core->nglobals)
        return JS_UNDEFINED;
    {
        unsigned long long raw = g->core->globals[g->index];

        switch (g->type) {
        case 0x7f: return JS_NewInt32(ctx, (int32_t)(uint32_t)raw);
        case 0x7e: return JS_NewInt64(ctx, (int64_t)raw);
        case 0x7d: {
            float f;
            uint32_t bits = (uint32_t)raw;
            memcpy(&f, &bits, sizeof(f));
            return JS_NewFloat64(ctx, (double)f);
        }
        case 0x7c: {
            double d;
            memcpy(&d, &raw, sizeof(d));
            return JS_NewFloat64(ctx, d);
        }
        default:   return JS_UNDEFINED;
        }
    }
}

static JSValue js_wasm_global_value_setter(JSContext *ctx, JSValueConst this_val,
                                           JSValueConst val)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_find(rt);
    JSClassID cid;
    WasmGlobalObj *g;
    unsigned long long raw = 0;

    g = JS_GetAnyOpaque(this_val, &cid);
    if (!cls || !g || cid != cls->global)
        return JS_ThrowTypeError(ctx, "not a WebAssembly.Global");
    if (!g->mutable_)
        return JS_ThrowTypeError(ctx, "global is immutable");
    if (!g->core->globals || g->index < 0 || g->index >= g->core->nglobals)
        return JS_UNDEFINED;
    switch (g->type) {
    case 0x7f: {
        int32_t v;

        if (JS_ToInt32(ctx, &v, val))
            return JS_EXCEPTION;
        raw = (uint32_t)v;
        break;
    }
    case 0x7e: {
        int64_t v;

        if (JS_ToInt64(ctx, &v, val))
            return JS_EXCEPTION;
        raw = (unsigned long long)v;
        break;
    }
    case 0x7d: {
        double d;

        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        {
            float f = (float)d;
            uint32_t bits;

            memcpy(&bits, &f, sizeof(bits));
            raw = bits;
        }
        break;
    }
    case 0x7c: {
        double d;

        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        memcpy(&raw, &d, sizeof(d));
        break;
    }
    default:
        return JS_UNDEFINED;
    }
    g->core->globals[g->index] = raw;
    /* Push it back to wasm3 too: a function that still runs there reads the
       M3Global cell, not this shadow. */
    wasm_globals_store(g->core);
    return JS_UNDEFINED;
}

/* Build the object for export `index` (a global index in the module's space). */
static JSValue wasm_global_object(JSContext *ctx, WasmCore *core, int index)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_find(rt);
    JSValue obj, getter, setter;
    WasmGlobalObj *g;
    JSAtom atom;
    int type = 0x7f;
    int is_mut = 0;

    if (!cls || !core->mod || index < 0 || (uint32_t)index >= core->mod->numGlobals)
        return JS_UNDEFINED;
    {
        M3Global *mg = &core->mod->globals[index];

        /* wasm3 keeps the value type as its own enum; map it back to the wasm
           byte the JS side expects. */
        switch (mg->type) {
        case c_m3Type_i32: type = 0x7f; break;
        case c_m3Type_i64: type = 0x7e; break;
        case c_m3Type_f32: type = 0x7d; break;
        case c_m3Type_f64: type = 0x7c; break;
        default:           type = 0x7f; break;
        }
        is_mut = mg->isMutable ? 1 : 0;
    }
    obj = JS_NewObjectClass(ctx, cls->global);
    if (JS_IsException(obj))
        return obj;
    g = js_mallocz(ctx, sizeof(*g));
    if (!g) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    g->jrt = rt;
    g->core = wasm_core_ref(core);
    g->index = index;
    g->type = type;
    g->mutable_ = is_mut;
    JS_SetOpaque(obj, g);
    atom = JS_NewAtom(ctx, "value");
    getter = JS_NewCFunction2(ctx, (JSCFunction *)js_wasm_global_value_getter,
                              "get value", 0, JS_CFUNC_getter, 0);
    setter = is_mut
        ? JS_NewCFunction2(ctx, (JSCFunction *)js_wasm_global_value_setter,
                           "set value", 1, JS_CFUNC_setter, 0)
        : JS_UNDEFINED;
    JS_DefinePropertyGetSet(ctx, obj, atom, getter, setter, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
    return obj;
}

/* ------------------------------------------------------------------ */
/* linear memory view                                                  */
/* ------------------------------------------------------------------ */
/* Drop the cached buffer if memory.grow moved the block. */
static void wasm_core_refresh_memory(JSContext *ctx, WasmCore *core)
{
    size_t size = 0;
    uint8_t *p;

    if (!core->mod)
        return;
    p = m3_GetMemory(core->mod, &size, 0);
    /* A NULL answer means wasm3 cannot describe the memory right now (it does
       that for a module whose memory has not been initialised yet). Keeping
       the last known block is the right move: it is the one the instance is
       still writing through, and overwriting the cache with NULL would leave
       the translated code with no memory at all. */
    if (!p)
        return;
    if (p == core->mem_ptr && size == core->mem_size)
        return;

    if (!JS_IsNull(core->mem_abuf)) {
        /* neuters every typed array built from this buffer */
        JS_DetachArrayBuffer(ctx, core->mem_abuf);
        JS_FreeValue(ctx, core->mem_view);
        JS_FreeValue(ctx, core->mem_abuf);
        core->mem_view = JS_NULL;
        core->mem_abuf = JS_NULL;
    }
    core->mem_ptr = p;
    core->mem_size = size;
}

/* Return a new reference to an ArrayBuffer over the current linear memory. */
static JSValue wasm_core_memory_buffer(JSContext *ctx, WasmCore *core)
{
    size_t size = 0;
    uint8_t *p;

    wasm_core_refresh_memory(ctx, core);
    p = core->mem_ptr;
    size = core->mem_size;
    if (!p || !size)
        return JS_NULL;
    if (!JS_IsNull(core->mem_abuf))
        return JS_DupValue(ctx, core->mem_abuf);

    /* the block belongs to wasm3: no realloc_func, so quickjs never frees it */
    core->mem_abuf = JS_NewArrayBuffer(ctx, p, size, 0, NULL, NULL, false);
    if (JS_IsException(core->mem_abuf)) {
        core->mem_abuf = JS_NULL;
        return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, core->mem_abuf);
}

static JSValue js_wasm_memory_buffer_getter(JSContext *ctx, JSValueConst this_val)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_find(rt);
    JSClassID cid;
    WasmMemory *m;

    m = JS_GetAnyOpaque(this_val, &cid);
    if (!cls || !m || cid != cls->memory)
        return JS_ThrowTypeError(ctx, "not a WebAssembly.Memory");
    return wasm_core_memory_buffer(ctx, m->core);
}

/* memory.grow(delta) -> previous page count, or -1 when it cannot grow.
 *
 * The memory belongs to the module, so the resize goes through wasm3's own
 * ResizeMemory: that keeps the interpreter and the translated code looking at
 * the same block. The cached ArrayBuffer is detached by
 * wasm_core_refresh_memory on the next access, which is what the spec asks for
 * (the old buffer is left detached at length 0). */
static JSValue js_wasm_memory_grow(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_find(rt);
    JSClassID cid;
    WasmMemory *m;
    WasmCore *core;
    int64_t delta = 0;
    IM3Memory mem;
    uint64_t old_pages;

    m = JS_GetAnyOpaque(this_val, &cid);
    if (!cls || !m || cid != cls->memory)
        return JS_ThrowTypeError(ctx, "not a WebAssembly.Memory");
    core = m->core;
    if (argc > 0 && JS_ToInt64(ctx, &delta, argv[0]))
        return JS_EXCEPTION;
    if (delta < 0)
        return JS_ThrowRangeError(ctx, "grow: delta must not be negative");
    if (!core->mod || core->mod->numMemories < 1)
        return JS_NewInt32(ctx, -1);
    mem = core->mod->memories[0];
    if (!mem)
        return JS_NewInt32(ctx, -1);
    old_pages = mem->numPages;
    /* wasm's limit is 65536 pages; ResizeMemory refuses anything past the
       declared maximum and returns an error, which the spec maps to -1. */
    if (delta > 0) {
        if (ResizeMemory(core->rt, mem, old_pages + (uint64_t)delta))
            return JS_NewInt32(ctx, -1);
        /* the block moved or grew: drop the stale view before anyone reads it */
        core->mem_ptr = NULL;
        core->mem_size = 0;
    }
    return JS_NewInt64(ctx, (int64_t)old_pages);
}

/* ------------------------------------------------------------------ */
/* exported function call                                              */
/* ------------------------------------------------------------------ */

/* The translated fast path.
 *
 * The signature is wasm's: up to 8 parameters of i32/i64/f32/f64 and at most
 * one result. The C entry takes (typed params..., uint8_t *mem, uint32_t
 * memsize), so arguments are marshalled into a 64-bit slot array here - the
 * generated code reads each slot through the parameter's declared type.
 *
 * Returns JS_EXCEPTION when a trap was raised, 0 for "no result", otherwise 1
 * with the value in *out. */
static int wasm_call_native(JSContext *ctx, WasmFn *wf, int argc,
                            JSValueConst *argv, JSValue *out)
{
    WasmCore *core = wf->core;
    /* Slots, not typed arguments: every translated function carries a wrapper
       that takes this one shape, so the call below is a single function pointer
       cast for any wasm signature - 0..8 parameters, any mix of the four types.
       A float's bits are copied, never converted: the slot is storage. */
    unsigned long long args[8];
    unsigned long long rslot = 0;
    uint8_t *mem = NULL;
    size_t memsize = 0;
    uint32_t i;
    uint32_t np = (uint32_t)wf->nparams;
    void *wentry;
    int tr = 0;

    if ((uint32_t)argc < np)
        return JS_ThrowTypeError(ctx, "wasm: expected %u arguments, got %d",
                                 np, argc), -1;

    for (i = 0; i < np; i++) {
        switch (wf->params[i]) {
        case 1: {
            int32_t n;
            if (JS_ToInt32(ctx, &n, argv[i]) < 0) return -1;
            args[i] = (uint32_t)n;
            break;
        }
        case 2: {
            int64_t n;
            if (JS_ToBigInt64(ctx, &n, argv[i]) < 0) return -1;
            args[i] = (uint64_t)n;
            break;
        }
        case 3: {
            double d;
            float f;
            uint32_t u;

            if (JS_ToFloat64(ctx, &d, argv[i]) < 0) return -1;
            f = (float)d;
            memcpy(&u, &f, 4);
            args[i] = u;
            break;
        }
        case 4: {
            double d;
            uint64_t v;

            if (JS_ToFloat64(ctx, &d, argv[i]) < 0) return -1;
            memcpy(&v, &d, 8);
            args[i] = v;
            break;
        }
        default:
            return JS_ThrowTypeError(ctx, "wasm: bad parameter type"), -1;
        }
    }

    if (whale_wasm_dbg())
        fprintf(stderr, "[wasm-call] np=%u p0=%d nr=%d r0=%d\n",
                np, np ? wf->params[0] : -1, wf->nresults,
                wf->nresults ? wf->results[0] : -1);
    /* The address is read from the instance's table on every call, never
       cached in the descriptor: a later instantiate relocates the shared TCC
       state and moves every function, and a stale pointer would be called.
       The generation check is what notices the relocate. */
    wasm_core_refresh_symbols(core);
    wentry = core->native[wf->native_index].wentry;
    if (!wentry)
        return JS_ThrowTypeError(ctx, "wasm: entry symbol went missing"), -1;
    /* The instance caches the linear memory pointer (wasm_core_refresh_memory
       keeps it current, including after a grow), and that is the pointer the
       translated code has to write through: it is the same block m3_GetMemory
       returns while the memory is live, but it does not depend on this call
       being able to ask wasm3 again. */
    wasm_core_refresh_memory(ctx, core);
    mem = core->mem_ptr;
    memsize = core->mem_size;

    whale_wasm_clear_trap();
    /* Translated loops poll the embedder's interrupt handler through this, and
       the context is per-call, so it is installed here rather than once. */
    whale_wasm_set_interrupt_probe(wasm_interrupt_probe, ctx);

    /* Globals: the shadow is what the translated code reads and writes. Pull it
       from wasm3 before the call so a function that still runs on the
       interpreter is seen, and push it back after so the interpreter sees what
       the native code did. */
    wasm_globals_load(core);
    ((void (*)(unsigned long long *, unsigned long long *, unsigned char *,
               unsigned int, unsigned long long *))wentry)(
        args, &rslot, mem, (unsigned int)memsize, core->globals);
    wasm_globals_store(core);

    if (whale_wasm_trapped() || whale_div_trapped()) {
        /* An interrupt stops a translated loop by returning through the trap
           path, so the two are told apart here: the handler already raised the
           error, and reporting a trap as well would replace it with the wrong
           message. */
        if (whale_wasm_interrupted()) {
            /* A translated loop stopped on request: the handler raised the
               error, and the instance is retired - the frame it left behind
               cannot be resumed, and its later calls must not pretend the
               program is still in a known state. */
            core->poisoned = 1;
            return -1;
        }
        return JS_ThrowTypeError(ctx,
            "wasm trap: out of bounds or integer divide"), -1;
    }

    switch (wf->nresults) {
    case 0:
        break;
    case 1:
        switch (wf->results[0]) {
        case 1: *out = JS_NewInt32(ctx, (int32_t)(uint32_t)rslot); break;
        case 2: *out = JS_NewBigInt64(ctx, (int64_t)rslot); break;
        case 3: {
            float f;
            uint32_t u = (uint32_t)rslot;

            memcpy(&f, &u, 4);
            *out = JS_NewFloat64(ctx, f);
            break;
        }
        case 4: {
            double d;

            memcpy(&d, &rslot, 8);
            *out = JS_NewFloat64(ctx, d);
            break;
        }
        default: *out = JS_UNDEFINED; break;
        }
        break;
    default:
        return JS_ThrowTypeError(ctx,
            "wasm: multi-value results are not supported"), -1;
    }

    /* a call that wrote to linear memory cannot move it (only grow can), but
       the cached view still has to be current */
    wasm_core_refresh_memory(ctx, core);
    return 0;
}

static JSValue js_wasm_call(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic,
                            void *opaque)
{
    WasmFn *wf = opaque;
    WasmCore *core = wf->core;
    IM3Function f = wf->fn;
    uint32_t nargs = m3_GetArgCount(f);
    uint32_t nrets = m3_GetRetCount(f);
    void **argp;
    uint64_t *store;
    M3Result r;
    uint32_t i;
    JSValue ret;

    (void)this_val; (void)magic;
    if (core->poisoned)
        return JS_ThrowTypeError(ctx,
            "wasm: instance stopped by an interrupt and can no longer run");
    if ((uint32_t)argc < nargs)
        return JS_ThrowTypeError(ctx, "wasm: expected %u arguments, got %d",
                                 nargs, argc);

    /* translated code first: it is the whole point of the translator. The
       generation check inside has to run before the entry is inspected, or a
       relocate that happened since the last call would leave this test looking
       at a stale pointer. */
    if (wf->native_index >= 0 && core->native) {
        wasm_core_refresh_symbols(core);
    }
    if (wf->native_index >= 0 && core->native &&
        core->native[wf->native_index].entry) {
        JSValue out = JS_UNDEFINED;
        int rc = wasm_call_native(ctx, wf, argc, argv, &out);

        if (rc < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        if (whale_wasm_dbg())
            fprintf(stderr, "[wasm-call] done\n");
        return out;
    }

    argp = js_malloc(ctx, sizeof(void *) * (nargs ? nargs : 1));
    store = js_malloc(ctx, sizeof(uint64_t) * (nargs ? nargs : 1));
    if (!argp || !store) {
        js_free(ctx, argp);
        js_free(ctx, store);
        return JS_ThrowOutOfMemory(ctx);
    }

    for (i = 0; i < nargs; i++) {
        switch (wasm_type(m3_GetArgType(f, i))) {
        case 1: {
            int32_t n;
            if (JS_ToInt32(ctx, &n, argv[i]) < 0) goto conv_fail;
            store[i] = (uint32_t)n;
            break;
        }
        case 2: {
            int64_t n;
            if (JS_ToBigInt64(ctx, &n, argv[i]) < 0) goto conv_fail;
            store[i] = (uint64_t)n;
            break;
        }
        case 3: {
            double d;
            if (JS_ToFloat64(ctx, &d, argv[i]) < 0) goto conv_fail;
            store[i] = wasm_slot_from_f32((float)d);
            break;
        }
        case 4: {
            double d;
            if (JS_ToFloat64(ctx, &d, argv[i]) < 0) goto conv_fail;
            memcpy(&store[i], &d, 8);
            break;
        }
        default:
            js_free(ctx, argp);
            js_free(ctx, store);
            return JS_ThrowTypeError(ctx, "wasm: unsupported argument type");
        }
        argp[i] = &store[i];
    }

    r = m3_Call(f, nargs, (const void **)argp);
    js_free(ctx, argp);
    js_free(ctx, store);

    /* A gas slice that ran out leaves the invocation paused, not dead. Poll the
       embedder's interrupt handler at each pause and either resume with a fresh
       slice or stop. This is the only place wasm3 hands control back, so it is
       the only place an interrupt can be noticed: m3_Yield is called at every
       function call, and a metered runtime pauses at every loop back edge. */
    while (r == m3Err_continuationSuspended) {
        if (JS_WhalePollInterrupt(ctx)) {
            /* The paused invocation cannot be abandoned in place - its
               continuation owns the runtime's value stack - and resuming it
               would be running the code the embedder just asked to stop. So the
               instance is retired: this call returns the interrupt error and
               every later call on it refuses. m3_FreeRuntime, at the instance's
               finalizer, releases the suspended continuation. */
            core->poisoned = 1;
            wasm_core_refresh_memory(ctx, core);
            return JS_EXCEPTION;    /* the interrupt error is already thrown */
        }
        m3_SetResourceLimit(core->rt, c_m3Limit_GasUnits, WASM_GAS_SLICE);
        r = m3_ResumeRuntime(core->rt);
    }

    /* a grow inside the call may have moved the block */
    wasm_core_refresh_memory(ctx, core);

    if (core->pending_exc) {
        core->pending_exc = 0;
        return JS_EXCEPTION;        /* the import's exception is already set */
    }
    if (r)
        return JS_ThrowTypeError(ctx, "wasm trap: %s", r);

    if (nrets == 0)
        return JS_UNDEFINED;
    if (nrets == 1) {
        switch (wasm_type(m3_GetRetType(f, 0))) {
        case 1: {
            int32_t n;
            if (m3_GetResults(f, 1, (const void *[]){ &n })) return JS_EXCEPTION;
            return JS_NewInt32(ctx, n);
        }
        case 2: {
            int64_t n;
            if (m3_GetResults(f, 1, (const void *[]){ &n })) return JS_EXCEPTION;
            return JS_NewBigInt64(ctx, n);
        }
        case 3: {
            float x;
            if (m3_GetResults(f, 1, (const void *[]){ &x })) return JS_EXCEPTION;
            return JS_NewFloat64(ctx, x);
        }
        case 4: {
            double d;
            if (m3_GetResults(f, 1, (const void *[]){ &d })) return JS_EXCEPTION;
            return JS_NewFloat64(ctx, d);
        }
        default:
            return JS_UNDEFINED;
        }
    }

    ret = JS_NewArray(ctx);
    if (JS_IsException(ret))
        return ret;
    {
        /* m3_GetResults fills every slot in one call, so collect the raw
           values first and convert afterwards */
        uint64_t *raw = js_malloc(ctx, sizeof(uint64_t) * nrets);
        const void **ptrs = js_malloc(ctx, sizeof(void *) * nrets);

        if (!raw || !ptrs) {
            js_free(ctx, raw);
            js_free(ctx, ptrs);
            JS_FreeValue(ctx, ret);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (i = 0; i < nrets; i++)
            ptrs[i] = &raw[i];
        if (m3_GetResults(f, nrets, ptrs)) {
            js_free(ctx, raw);
            js_free(ctx, ptrs);
            JS_FreeValue(ctx, ret);
            return JS_ThrowTypeError(ctx, "wasm: cannot read results");
        }
        for (i = 0; i < nrets; i++) {
            JSValue v = JS_UNDEFINED;

            switch (wasm_type(m3_GetRetType(f, i))) {
            case 1: v = JS_NewInt32(ctx, (int32_t)(uint32_t)raw[i]); break;
            case 2: v = JS_NewBigInt64(ctx, (int64_t)raw[i]); break;
            case 3: v = JS_NewFloat64(ctx, wasm_slot_f32(raw[i])); break;
            case 4: {
                double d;
                memcpy(&d, &raw[i], 8);
                v = JS_NewFloat64(ctx, d);
                break;
            }
            }
            JS_SetPropertyUint32(ctx, ret, i, v);
        }
        js_free(ctx, raw);
        js_free(ctx, ptrs);
    }
    return ret;

  conv_fail:
    js_free(ctx, argp);
    js_free(ctx, store);
    return JS_EXCEPTION;
}

/* ------------------------------------------------------------------ */
/* minimal wasm reader, for the export section                         */
/* ------------------------------------------------------------------ */
typedef struct WasmReader {
    const uint8_t *p;
    const uint8_t *end;
    int ok;
} WasmReader;

static uint32_t wasm_uleb(WasmReader *r)
{
    uint32_t v = 0;
    int shift = 0;

    while (r->p < r->end) {
        uint8_t b = *r->p++;

        v |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
        shift += 7;
        if (shift > 35)
            break;
    }
    r->ok = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* module construction                                                 */
/* ------------------------------------------------------------------ */
/* Validate the bytes and wrap a copy in a Module object. */
static JSValue wasm_module_new(JSContext *ctx, JSValueConst bytes)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_get(rt);
    size_t len = 0;
    uint8_t *data;
    JSValue obj, copy;
    WasmModule *m;
    IM3Environment env;
    IM3Module mod;
    M3Result r;

    if (!cls)
        return JS_ThrowInternalError(ctx, "wasm: no class slot left");

    data = JS_GetArrayBuffer(ctx, &len, bytes);
    if (!data)
        data = JS_GetUint8Array(ctx, &len, bytes);
    if (!data)
        return JS_ThrowTypeError(ctx,
            "WebAssembly: expected an ArrayBuffer or Uint8Array");
    if (len < 8)
        return JS_ThrowTypeError(ctx, "WebAssembly: module too short");

    /* validate once, so compile() reports a bad module immediately */
    env = m3_NewEnvironment();
    if (!env)
        return JS_ThrowOutOfMemory(ctx);
    r = m3_ParseModule(env, &mod, data, (uint32_t)len);
    if (r) {
        m3_FreeEnvironment(env);
        return JS_ThrowTypeError(ctx, "WebAssembly: %s", r);
    }
    m3_FreeModule(mod);
    m3_FreeEnvironment(env);

    obj = JS_NewObjectClass(ctx, cls->module);
    if (JS_IsException(obj))
        return obj;

    m = js_mallocz(ctx, sizeof(*m));
    if (!m) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    copy = JS_NewArrayBufferCopy(ctx, data, len);
    if (JS_IsException(copy)) {
        js_free(ctx, m);
        JS_FreeValue(ctx, obj);
        return copy;
    }
    m->jrt = rt;
    m->bytes = copy;
    JS_SetOpaque(obj, m);
    return obj;
}

/* Link every function the imports object provides. */
static void wasm_link_imports(JSContext *ctx, WasmCore *core, JSValueConst imports)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;

    if (!JS_IsObject(imports))
        return;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, imports,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return;

    for (i = 0; i < n; i++) {
        const char *modname = JS_AtomToCString(ctx, tab[i].atom);
        JSValue modval = JS_GetProperty(ctx, imports, tab[i].atom);
        JSPropertyEnum *tab2 = NULL;
        uint32_t n2 = 0, j;

        if (modname && JS_IsObject(modval) &&
            JS_GetOwnPropertyNames(ctx, &tab2, &n2, modval,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (j = 0; j < n2; j++) {
                const char *fname = JS_AtomToCString(ctx, tab2[j].atom);
                JSValue fnv = JS_GetProperty(ctx, modval, tab2[j].atom);

                if (fname && JS_IsFunction(ctx, fnv)) {
                    WasmImportLink *link = js_mallocz(ctx, sizeof(*link));

                    if (link) {
                        link->fn = JS_DupValue(ctx, fnv);
                        link->core = core;
                        link->next = core->imports;
                        core->imports = link;
                        /* the trampoline reads the signature from the
                           IM3Function, so no signature string is needed */
                        m3_LinkRawFunctionEx(core->mod, modname, fname, NULL,
                                             js_wasm_trampoline, link);
                    }
                }
                JS_FreeValue(ctx, fnv);
                if (fname)
                    JS_FreeCString(ctx, fname);
            }
            JS_FreePropertyEnum(ctx, tab2, n2);
        }
        JS_FreeValue(ctx, modval);
        if (modname)
            JS_FreeCString(ctx, modname);
    }
    JS_FreePropertyEnum(ctx, tab, n);
}

/* ------------------------------------------------------------------ */
/* the translated fast path                                            */
/* ------------------------------------------------------------------ */
/* Scan the module's type/function/code sections and translate every function
 * it can. Returns a dense table of func_count entries, or NULL when the module
 * has no code to translate. Free with free(). */
static NativeFn *wasm_translate_module(JSContext *ctx, WasmCore *core, int *count)
{
    size_t len = 0;
    uint8_t *buf;
    WasmReader r;
    const uint8_t *code_sec = NULL, *code_end = NULL;
    const uint8_t *type_sec = NULL;
    const uint8_t *func_sec = NULL;
    const uint8_t *import_sec = NULL;
    const uint8_t *global_sec = NULL;
    uint32_t func_count = 0;
    uint32_t imported_funcs = 0;
    uint32_t imported_globals = 0;
    static int imported_global_type[128];
    NativeFn *tab;

    *count = 0;
    buf = JS_GetArrayBuffer(ctx, &len, core->bytes);
    if (!buf || len < 8 || memcmp(buf, "\0asm", 4))
        return NULL;

    r.p = buf + 8;
    r.end = buf + len;
    r.ok = 1;
    while (r.p < r.end && r.ok) {
        uint8_t id = *r.p++;
        uint32_t size = wasm_uleb(&r);
        const uint8_t *sec;

        if (!r.ok || (size_t)(r.end - r.p) < size)
            break;
        sec = r.p;
        r.p += size;
        if (id == 1) type_sec = sec;
        else if (id == 2) import_sec = sec;
        else if (id == 3) func_sec = sec;
        else if (id == 6) global_sec = sec;
        else if (id == 10) { code_sec = sec; code_end = sec + size; }
    }
    if (!type_sec || !func_sec || !code_sec)
        return NULL;

    /* The function index space puts imports first: an export naming function 3
       in a module with 3 imported functions is the first *defined* body. The
       code section only holds the defined ones, so the table has to be shifted
       by the import count. */
    if (import_sec) {
        WasmReader ir;
        uint32_t n, j;

        ir.p = import_sec;
        ir.end = r.end;
        ir.ok = 1;
        n = wasm_uleb(&ir);
        for (j = 0; j < n && ir.ok; j++) {
            uint32_t mlen = wasm_uleb(&ir);     /* module name length */
            uint32_t flen;
            uint8_t kind;

            if (!ir.ok || (size_t)(r.end - ir.p) < mlen)
                break;
            ir.p += mlen;                       /* skip the module name */
            flen = wasm_uleb(&ir);              /* field name length */
            if (!ir.ok || (size_t)(r.end - ir.p) < flen)
                break;
            ir.p += flen;                       /* skip the field name */
            kind = *ir.p++;
            if (kind == 0) {            /* function import */
                (void)wasm_uleb(&ir);   /* type index */
                imported_funcs++;
            } else if (kind == 1) {     /* table */
                uint8_t elem = *ir.p++;
                uint32_t lo = wasm_uleb(&ir);
                if (lo & 1)
                    (void)wasm_uleb(&ir);
                (void)elem;
            } else if (kind == 2) {     /* memory */
                uint32_t lo = wasm_uleb(&ir);
                if (lo & 1)
                    (void)wasm_uleb(&ir);
            } else if (kind == 3) {     /* global */
                uint8_t bt = ir.p[0];

                /* The imported globals occupy the front of the index space, so
                   their types have to be recorded where the translator will
                   look for them. */
                if (imported_globals < 128) {
                    int t = (bt == 0x7f) ? 1 : (bt == 0x7e) ? 2 :
                            (bt == 0x7d) ? 3 : (bt == 0x7c) ? 4 : 0;

                    imported_global_type[imported_globals] = t;
                }
                imported_globals++;
                ir.p += 2;              /* valtype, mutability */
            }
        }
    }

    {
        WasmReader fr;
        uint32_t n;

        fr.p = func_sec;
        fr.end = r.end;
        fr.ok = 1;
        n = wasm_uleb(&fr);
        if (!fr.ok || n == 0)
            return NULL;
        func_count = n;
    }

    /* The table is indexed by the module's function index, which counts the
       imported functions first, so it needs room for all of them - not just
       the bodies in the code section. Allocating only func_count and indexing
       at f + imported_funcs wrote past the end of the block. */
    tab = calloc(func_count + imported_funcs, sizeof(*tab));
    if (!tab)
        return NULL;
    *count = (int)(func_count + imported_funcs);
    if (whale_wasm_dbg())
        fprintf(stderr, "[wasm-c] funcs=%u imported=%u\n", func_count, imported_funcs);

    {
        /* type table: index -> signature */
        static int t_params[512][8], t_pc[512], t_rc[512], t_ret[512][4];
        WasmReader tr;
        uint32_t type_count, k;
        /* Translated code accumulates in ONE TCCState, so a symbol name has to
           be unique across every module ever instantiated - not just within
           this one. */
        static unsigned module_seq;

        tr.p = type_sec;
        tr.end = r.end;
        tr.ok = 1;
        type_count = wasm_uleb(&tr);
        for (k = 0; k < type_count && k < 512 && tr.ok; k++) {
            uint32_t np, nr, j;
            uint8_t form = *tr.p++;

            if (form != 0x60) { tr.ok = 0; break; }
            np = wasm_uleb(&tr);
            if (np > 8) { tr.ok = 0; break; }
            for (j = 0; j < np; j++) {
                uint8_t bt = *tr.p++;

                t_params[k][j] = (bt == 0x7f) ? 1 : (bt == 0x7e) ? 2 :
                                 (bt == 0x7d) ? 3 : (bt == 0x7c) ? 4 : 0;
            }
            t_pc[k] = (int)np;
            nr = wasm_uleb(&tr);
            if (nr > 4) { tr.ok = 0; break; }
            for (j = 0; j < nr; j++) {
                uint8_t bt = *tr.p++;

                t_ret[k][j] = (bt == 0x7f) ? 1 : (bt == 0x7e) ? 2 :
                              (bt == 0x7d) ? 3 : (bt == 0x7c) ? 4 : 0;
            }
            t_rc[k] = (int)nr;
        }

        if (tr.ok) {
            WasmReader fr, cr;
            uint32_t f;
            unsigned seq = ++module_seq;
            int any = 0;
            void *tmod;
            /* Every function's signature first: a `call` inside a body needs
               the callee's signature even before the callee is translated. */
            int pass;

            tmod = JS_WhaleWasmModuleNew((int)(func_count + imported_funcs));
            if (!tmod) {
                free(tab);
                return NULL;
            }
            /* imported globals come first in the global index space */
            {
                uint32_t g;

                for (g = 0; g < imported_globals && g < 128; g++)
                    JS_WhaleWasmModuleSetGlobal(tmod, (int)g,
                                                imported_global_type[g]);
            }

            fr.p = func_sec;
            fr.end = r.end;
            fr.ok = 1;
            (void)wasm_uleb(&fr);

            cr.p = code_sec;
            cr.end = code_end;
            cr.ok = 1;
            (void)wasm_uleb(&cr);

            for (f = 0; f < func_count && fr.ok; f++) {
                uint32_t ti = wasm_uleb(&fr);
                uint32_t body_size = wasm_uleb(&cr);
                const uint8_t *body = cr.p;
                uint32_t idx;
                int np, nr, j, pi[8], ri[4];
                char sym[40];

                if (!fr.ok || !cr.ok || (size_t)(code_end - body) < body_size)
                    break;
                cr.p += body_size;

                if (ti >= type_count || ti >= 512)
                    continue;
                np = t_pc[ti];
                nr = t_rc[ti];
                for (j = 0; j < np; j++) pi[j] = t_params[ti][j];
                for (j = 0; j < nr; j++) ri[j] = t_ret[ti][j];
                idx = f + imported_funcs;
                snprintf(sym, sizeof(sym), "whale_wasm_%u_%u", seq, idx);
                JS_WhaleWasmModuleSetFunc(tmod, (int)idx, pi, np, ri, nr, sym);
                tab[idx].nparams = np;
                tab[idx].nresults = nr;
                for (j = 0; j < np; j++) tab[idx].params[j] = pi[j];
                for (j = 0; j < nr; j++) tab[idx].results[j] = ri[j];
                snprintf(tab[idx].sym, sizeof(tab[idx].sym), "%s", sym);
                /* the uniform entry wrapper the binding actually calls */
                snprintf(tab[idx].wsym, sizeof(tab[idx].wsym), "%s__w", sym);
            }

            /* The global section: the translator needs each global's value type
               to emit the right accessor. Imports come first in the index
               space, so they are counted before the defined ones. */
            if (global_sec) {
                WasmReader gr;
                uint32_t n, g;

                gr.p = global_sec;
                gr.end = r.end;
                gr.ok = 1;
                n = wasm_uleb(&gr);
                for (g = 0; g < n && gr.ok; g++) {
                    uint8_t bt = *gr.p++;
                    int t = (bt == 0x7f) ? 1 : (bt == 0x7e) ? 2 :
                            (bt == 0x7d) ? 3 : (bt == 0x7c) ? 4 : 0;
                    uint8_t mut = *gr.p++;

                    (void)mut;
                    /* skip the init expression, up to its end opcode */
                    while (gr.p < gr.end && *gr.p != 0x0b)
                        gr.p++;
                    if (gr.p < gr.end)
                        gr.p++;
                    JS_WhaleWasmModuleSetGlobal(tmod,
                        (int)(g + imported_globals), t);
                }
            }

            /* Translate to a fixpoint: a body that calls a sibling is refused
               until the sibling itself translated, and a sibling may be later
               in the section. Bounded by the function count, since each round
               either translates at least one more function or stops. */
            for (pass = 0; pass <= (int)func_count; pass++) {
                int progressed = 0;

                fr.p = func_sec;
                fr.end = r.end;
                fr.ok = 1;
                (void)wasm_uleb(&fr);

                cr.p = code_sec;
                cr.end = code_end;
                cr.ok = 1;
                (void)wasm_uleb(&cr);

                for (f = 0; f < func_count && fr.ok && cr.ok; f++) {
                    uint32_t ti = wasm_uleb(&fr);
                    uint32_t body_size = wasm_uleb(&cr);
                    const uint8_t *body = cr.p;
                    uint32_t idx;

                    if (!fr.ok || !cr.ok || (size_t)(code_end - body) < body_size)
                        break;
                    cr.p += body_size;
                    if (ti >= type_count || ti >= 512)
                        continue;
                    if (t_rc[ti] > 1)
                        continue;       /* no multi-value ABI */
                    idx = f + imported_funcs;
                    if (tab[idx].entry)
                        continue;
                    if (JS_WhaleWasmTranslate(tab[idx].sym, body, body_size,
                                              tab[idx].nparams, tab[idx].params,
                                              tab[idx].nresults, tab[idx].results,
                                              tmod, (int)idx)) {
                        /* the address is resolved after the batch relocate */
                        tab[idx].entry = (void *)1;
                        JS_WhaleWasmModuleMarkTranslated(tmod, (int)idx);
                        any = 1;
                        progressed = 1;
                    }
                }
                if (!progressed)
                    break;
            }
            JS_WhaleWasmModuleFree(tmod);

            /* One relocate for the whole module. Doing it per function would
               move every function compiled before it, and the pointers already
               handed out would dangle. */
            if (any) {
                if (!JS_WhaleRelocateExtern()) {
                    uint32_t k;

                    for (k = 0; k < func_count; k++)
                        tab[k + imported_funcs].entry = NULL;
                } else {
                    /* This module's own table, resolved in the generation this
                       relocate produced. Other instances are not touched here:
                       each re-resolves lazily on its next call, because an
                       instance may belong to a runtime that no longer exists. */
                    uint32_t k;

                    for (k = 0; k < func_count; k++) {
                        uint32_t slot = k + imported_funcs;

                        if (!tab[slot].entry)
                            continue;
                        tab[slot].entry = JS_WhaleFindSymbol(tab[slot].sym);
                        tab[slot].wentry = JS_WhaleFindSymbol(tab[slot].wsym);
                        if (whale_wasm_dbg())
                            fprintf(stderr, "[wasm-c] resolve %s=%p %s=%p\n",
                                    tab[slot].sym, tab[slot].entry,
                                    tab[slot].wsym, tab[slot].wentry);
                        /* the wrapper is what the binding calls; a function
                           whose wrapper did not resolve cannot be used, and
                           leaving entry set would claim it can */
                        if (!tab[slot].wentry)
                            tab[slot].entry = NULL;
                    }
                }
            }
        }
    }
    return tab;
}

/* Build Instance.exports from the module's export section. */
static JSValue wasm_build_exports(JSContext *ctx, WasmCore *core)
{
    WasmClasses *cls = wasm_classes_get(core->jrt);
    JSValue exports = JS_NewObject(ctx);
    WasmReader r;
    size_t len = 0;
    uint8_t *buf;
    int have_memory = 0;

    if (JS_IsException(exports))
        return exports;

    buf = JS_GetArrayBuffer(ctx, &len, core->bytes);
    if (!buf)
        return exports;

    r.p = buf;
    r.end = buf + len;
    r.ok = 1;
    if (len < 8 || memcmp(buf, "\0asm", 4) != 0) {
        return exports;
    }
    r.p = buf + 8;

    while (r.p < r.end && r.ok) {
        uint8_t id = *r.p++;
        uint32_t size = wasm_uleb(&r);
        const uint8_t *sec_end;

        if (!r.ok || (size_t)(r.end - r.p) < size) {
            r.ok = 0;
            break;
        }
        sec_end = r.p + size;

        if (id == 7) {              /* export section */
            uint32_t count = wasm_uleb(&r);
            uint32_t k;

            for (k = 0; k < count && r.ok && r.p < sec_end; k++) {
                uint32_t nlen = wasm_uleb(&r);
                const uint8_t *np;
                uint8_t kind;
                uint32_t index;
                char name[256];

                if (!r.ok || nlen >= sizeof(name) ||
                    (size_t)(sec_end - r.p) < nlen) {
                    r.ok = 0;
                    break;
                }
                np = r.p;
                r.p += nlen;
                kind = *r.p++;
                index = wasm_uleb(&r);
                if (!r.ok)
                    break;
                memcpy(name, np, nlen);
                name[nlen] = '\0';
                if (whale_wasm_dbg())
                    fprintf(stderr, "[export] kind=%u idx=%u name=%s\n",
                            kind, index, name);

                if (kind == 0) {            /* function */
                    IM3Function f = NULL;
                    M3Result fr = m3_FindFunctionIn(&f, core->mod, name);

                    if (whale_wasm_dbg() && fr)
                        fprintf(stderr, "[export] %s find failed: %s\n", name, fr);
                    if (fr == m3Err_none && f) {
                        WasmFn *wf = js_mallocz(ctx, sizeof(*wf));

                        if (wf) {
                            int j;

                            wf->jrt = core->jrt;
                            wf->core = wasm_core_ref(core);
                            wf->fn = f;
                            wf->native_index = -1;
                            /* a translated body, when the translator handled
                               this function index. The table is dense over the
                               module's function index space, and `index` from
                               the export section is in that same space - imports
                               included - so no adjustment is needed. */
                            if (core->native && (int)index < core->native_count &&
                                core->native[index].entry) {
                                wf->native_index = (int)index;
                                wf->nparams = core->native[index].nparams;
                                wf->nresults = core->native[index].nresults;
                                for (j = 0; j < wf->nparams; j++)
                                    wf->params[j] = core->native[index].params[j];
                                for (j = 0; j < wf->nresults; j++)
                                    wf->results[j] = core->native[index].results[j];
                                if (whale_wasm_dbg())
                                    fprintf(stderr, "[wasm] %s -> native (np=%d nr=%d r0=%d)\n",
                                            name, wf->nparams, wf->nresults,
                                            wf->nresults ? wf->results[0] : -1);
                            } else if (whale_wasm_dbg()) {
                                fprintf(stderr, "[wasm] %s -> wasm3\n", name);
                            }
                            JS_SetPropertyStr(ctx, exports, name,
                                JS_NewCClosure(ctx, js_wasm_call, name,
                                               js_wasm_fn_finalizer, 0, 0, wf));
                        }
                    }
                } else if (kind == 2 && !have_memory) {     /* memory */
                    JSValue obj = JS_NewObjectClass(ctx, cls->memory);
                    WasmMemory *mem = js_mallocz(ctx, sizeof(*mem));

                    if (mem && !JS_IsException(obj)) {
                        JSAtom atom;
                        JSValue getter;

                        mem->jrt = core->jrt;
                        mem->core = wasm_core_ref(core);
                        JS_SetOpaque(obj, mem);
                        atom = JS_NewAtom(ctx, "buffer");
                        getter = JS_NewCFunction2(ctx,
                            (JSCFunction *)js_wasm_memory_buffer_getter,
                            "get buffer", 0, JS_CFUNC_getter, 0);
                        JS_DefinePropertyGetSet(ctx, obj, atom, getter,
                                                JS_UNDEFINED, JS_PROP_CONFIGURABLE);
                        JS_FreeAtom(ctx, atom);
                        /* memory.grow: the spec's method, and the one a loader
                           that sizes its memory tightly needs at startup. */
                        JS_SetPropertyStr(ctx, obj, "grow",
                            JS_NewCFunction(ctx, js_wasm_memory_grow, "grow", 1));
                        JS_SetPropertyStr(ctx, exports, name, obj);
                        have_memory = 1;
                    } else {
                        js_free(ctx, mem);
                        JS_FreeValue(ctx, obj);
                    }
                } else if (kind == 3) {                 /* global */
                    JSValue g = wasm_global_object(ctx, core, (int)index);

                    if (!JS_IsUndefined(g) && !JS_IsException(g))
                        JS_SetPropertyStr(ctx, exports, name, g);
                    else
                        JS_FreeValue(ctx, g);
                }
            }
        }
        r.p = sec_end;
    }
    return exports;
}

/* Instantiate a Module object. */
static JSValue wasm_instantiate_module(JSContext *ctx, JSValueConst module_obj,
                                       JSValueConst imports)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_find(rt);
    JSClassID cid;
    WasmModule *m;
    WasmCore *core;
    JSValue obj, exports;
    size_t len = 0;
    uint8_t *data;
    M3Result r;

    if (!cls)
        return JS_ThrowInternalError(ctx, "wasm: not installed");
    m = JS_GetAnyOpaque(module_obj, &cid);
    if (!m || cid != cls->module)
        return JS_ThrowTypeError(ctx, "WebAssembly.instantiate: expected a Module");

    data = JS_GetArrayBuffer(ctx, &len, m->bytes);
    if (!data)
        return JS_ThrowTypeError(ctx, "WebAssembly.instantiate: bad module bytes");

    obj = JS_NewObjectClass(ctx, cls->instance);
    if (whale_wasm_dbg())
        fprintf(stderr, "[inst-mod] obj exc=%d\n", JS_IsException(obj));
    if (JS_IsException(obj))
        return obj;
    core = js_mallocz(ctx, sizeof(*core));
    if (!core) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    core->refs = 1;
    core->jrt = rt;
    core->ctx = ctx;
    core->bytes = JS_DupValue(ctx, m->bytes);
    core->mem_abuf = JS_NULL;
    core->mem_view = JS_NULL;

    core->env = m3_NewEnvironment();
    if (!core->env) {
        JS_FreeValue(ctx, obj);
        wasm_core_unref(core);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = m3_ParseModule(core->env, &core->mod, data, (uint32_t)len);
    if (r) {
        JS_FreeValue(ctx, obj);
        wasm_core_unref(core);
        return JS_ThrowTypeError(ctx, "WebAssembly.instantiate: %s", r);
    }
    core->rt = m3_NewRuntime(core->env, 512 * 1024, NULL);
    if (!core->rt) {
        JS_FreeValue(ctx, obj);
        wasm_core_unref(core);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = m3_LoadModule(core->rt, core->mod);
    if (r) {
        JS_FreeValue(ctx, obj);
        wasm_core_unref(core);
        return JS_ThrowTypeError(ctx, "WebAssembly.instantiate: %s", r);
    }

    /* The shadow the translated code indexes. Filled after loading, because
       the initialisers only run there. */
    wasm_globals_init(core);

    /* Arm the runtime before anything compiles: gas metering is a compile-time
       decision (IsMetering reads gasLimit while the body is emitted), and
       m3_FindFunctionIn compiles lazily, so a runtime armed afterwards would
       have unmetered code with no pause points at all. */
    m3_SetSuspendable(core->rt, true);
    m3_SetResourceLimit(core->rt, c_m3Limit_GasUnits, WASM_GAS_SLICE);

    wasm_link_imports(ctx, core, imports);

    /* Translate what the translator can handle into native code. Anything it
       declines keeps its wasm3 body, and the descriptor points at whichever
       exists - so a partial translation is a speed difference, not a
       correctness difference.
       WHALE_WASM_NOTRANSLATE is a diagnostic switch: it forces every function
       onto wasm3, which is how the two paths are compared without changing the
       module. */
    if (getenv("WHALE_WASM_NOTRANSLATE"))
        core->native = NULL;
    else
        core->native = wasm_translate_module(ctx, core, &core->native_count);
    /* the table was resolved in the generation the translate step produced */
    core->sym_gen = JS_WhaleJitGeneration();

    exports = wasm_build_exports(ctx, core);
    if (JS_IsException(exports)) {
        JS_FreeValue(ctx, obj);
        wasm_core_unref(core);
        return exports;
    }
    JS_SetPropertyStr(ctx, obj, "exports", exports);

    /* refresh once so the memory pointer is known */
    wasm_core_refresh_memory(ctx, core);

    if (whale_wasm_dbg())
        fprintf(stderr, "[inst-mod] setopaque=%d class=%u\n",
                JS_SetOpaque(obj, core), cls->instance);
    return obj;
}

/* Settle a value that may be a promise, then run `fn` with the result.
 *
 * `fn` is a C callback, not a JS function: the streaming APIs need to compile
 * the bytes they receive, and compiling is a C operation. So this wraps the
 * callback in a JS closure (`JS_NewCClosure` carries the pointer) and calls
 * `Promise.resolve(value).then(closure)`. Using the engine's own promise keeps
 * ordering and rejection propagation right - a hand-rolled job would have to
 * re-implement both. */
typedef JSValue (*WasmValueFn)(JSContext *ctx, void *opaque, JSValueConst value);

typedef struct WasmThunk {
    WasmValueFn fn;
    void *opaque;
} WasmThunk;

static JSValue wasm_then_thunk(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic, void *opaque)
{
    WasmThunk *t = opaque;

    (void)this_val; (void)magic;
    if (!t)
        return JS_ThrowTypeError(ctx, "wasm: thunk lost its callback");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "wasm: missing value");
    return t->fn(ctx, t->opaque, argv[0]);
}

static void wasm_thunk_free(void *opaque)
{
    WasmThunk *t = opaque;

    /* js_free needs a context; the finalizer has none, and the struct is
       tiny - leaking one allocation per streaming call would still be wrong,
       so it goes through free() rather than js_free. */
    free(t);
}

static JSValue wasm_then(JSContext *ctx, JSValueConst value, WasmValueFn fn,
                         void *opaque)
{
    JSValue global, promise_ctor, resolved, thenfn, thunk, out, args[1];
    WasmThunk *t;

    global = JS_GetGlobalObject(ctx);
    promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, promise_ctor)) {
        JS_FreeValue(ctx, promise_ctor);
        return JS_ThrowInternalError(ctx, "wasm: no Promise");
    }
    {
        JSValue resolve = JS_GetPropertyStr(ctx, promise_ctor, "resolve");

        if (!JS_IsFunction(ctx, resolve)) {
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, promise_ctor);
            return JS_ThrowInternalError(ctx, "wasm: no Promise.resolve");
        }
        args[0] = JS_DupValue(ctx, value);
        resolved = JS_Call(ctx, resolve, promise_ctor, 1, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, resolve);
    }
    JS_FreeValue(ctx, promise_ctor);
    if (JS_IsException(resolved))
        return resolved;
    t = malloc(sizeof(*t));
    if (!t) {
        JS_FreeValue(ctx, resolved);
        return JS_ThrowOutOfMemory(ctx);
    }
    t->fn = fn;
    t->opaque = opaque;
    thunk = JS_NewCClosure(ctx, wasm_then_thunk, "then",
                           wasm_thunk_free, 1, 0, t);
    if (JS_IsException(thunk)) {
        free(t);
        JS_FreeValue(ctx, resolved);
        return thunk;
    }
    thenfn = JS_GetPropertyStr(ctx, resolved, "then");
    if (!JS_IsFunction(ctx, thenfn)) {
        JS_FreeValue(ctx, thenfn);
        JS_FreeValue(ctx, thunk);
        JS_FreeValue(ctx, resolved);
        return JS_ThrowInternalError(ctx, "wasm: promise without then");
    }
    args[0] = thunk;
    out = JS_Call(ctx, thenfn, resolved, 1, args);
    JS_FreeValue(ctx, thenfn);
    JS_FreeValue(ctx, thunk);
    JS_FreeValue(ctx, resolved);
    return out;
}

/* WebAssembly.instantiateStreaming(source, imports) -> Promise<{module, instance}>
 * and compileStreaming(source) -> Promise<Module>.
 *
 * The spec's source is a Response (or a promise of one) whose body is read as
 * an ArrayBuffer. Both are pure JS-level glue: the engine exposes the sync
 * `compile`/`instantiate`, and the async contract is what a loader actually
 * calls. WhaleNode's mod_wasm_compat.c supplies these when the binding does not;
 * providing them here means a bare embedder (no Node surface) gets them too. */
static JSValue wasm_compile_from_value(JSContext *ctx, void *opaque, JSValueConst value)
{
    (void)opaque;
    return wasm_module_new(ctx, value);
}

static JSValue js_wasm_compile_streaming(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSValue body, out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "compileStreaming: argument required");
    if (JS_IsObject(argv[0])) {
        body = JS_GetPropertyStr(ctx, argv[0], "arrayBuffer");
        if (JS_IsFunction(ctx, body)) {
            JSValue buf = JS_Call(ctx, body, argv[0], 0, NULL);

            JS_FreeValue(ctx, body);
            body = buf;
        } else {
            JS_FreeValue(ctx, body);
            body = JS_DupValue(ctx, argv[0]);
        }
    } else {
        body = JS_DupValue(ctx, argv[0]);
    }
    if (JS_IsException(body))
        return body;
    out = wasm_then(ctx, body, wasm_compile_from_value, NULL);
    JS_FreeValue(ctx, body);
    return out;
}

typedef struct WasmInstOpaque {
    JSValue imports;
} WasmInstOpaque;

static JSValue wasm_instantiate_from_value(JSContext *ctx, void *opaque, JSValueConst value)
{
    WasmInstOpaque *o = opaque;
    JSValue mod = wasm_module_new(ctx, value);
    JSValue inst, ret;

    if (JS_IsException(mod))
        return mod;
    inst = wasm_instantiate_module(ctx, mod, o->imports);
    if (JS_IsException(inst)) {
        JS_FreeValue(ctx, mod);
        return inst;
    }
    ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "module", mod);
    JS_SetPropertyStr(ctx, ret, "instance", inst);
    return ret;
}

static JSValue js_wasm_instantiate_streaming(JSContext *ctx, JSValueConst this_val,
                                             int argc, JSValueConst *argv)
{
    JSValue body, out;
    WasmInstOpaque o;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "instantiateStreaming: argument required");
    if (JS_IsObject(argv[0])) {
        body = JS_GetPropertyStr(ctx, argv[0], "arrayBuffer");
        if (JS_IsFunction(ctx, body)) {
            JSValue buf = JS_Call(ctx, body, argv[0], 0, NULL);

            JS_FreeValue(ctx, body);
            body = buf;
        } else {
            JS_FreeValue(ctx, body);
            body = JS_DupValue(ctx, argv[0]);
        }
    } else {
        body = JS_DupValue(ctx, argv[0]);
    }
    if (JS_IsException(body))
        return body;
    o.imports = argc > 1 ? argv[1] : JS_UNDEFINED;
    out = wasm_then(ctx, body, wasm_instantiate_from_value, &o);
    JS_FreeValue(ctx, body);
    return out;
}

/* ------------------------------------------------------------------ */
/* WebAssembly namespace                                               */
/* ------------------------------------------------------------------ */
static JSValue js_wasm_validate(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    size_t len = 0;
    uint8_t *data;
    IM3Environment env;
    IM3Module mod;
    M3Result r;

    (void)this_val; (void)argc;
    data = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!data)
        data = JS_GetUint8Array(ctx, &len, argv[0]);
    if (!data)
        return JS_FALSE;

    env = m3_NewEnvironment();
    if (!env)
        return JS_FALSE;
    r = m3_ParseModule(env, &mod, data, (uint32_t)len);
    if (!r) {
        m3_FreeModule(mod);
        m3_FreeEnvironment(env);
        return JS_TRUE;
    }
    m3_FreeEnvironment(env);
    return JS_FALSE;
}

static JSValue js_wasm_compile(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv);

static JSValue js_wasm_instantiate(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    WasmClasses *cls = wasm_classes_find(rt);
    JSClassID cid;
    JSValue imports = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue inst;

    (void)this_val;
    if (whale_wasm_dbg())
        fprintf(stderr, "[inst] enter argc=%d tag0=%d cls=%p\n",
                argc, JS_VALUE_GET_TAG(argv[0]), (void *)cls);
    if (!cls)
        return JS_ThrowInternalError(ctx, "wasm: not installed");

    /* WebAssembly.instantiate(module, imports) -> Instance */
    if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
        JS_GetAnyOpaque(argv[0], &cid);
        if (whale_wasm_dbg())
            fprintf(stderr, "[inst] tag=obj cid=%u want=%u\n", cid, cls->module);
        if (cid == cls->module)
            return wasm_instantiate_module(ctx, argv[0], imports);
    } else if (whale_wasm_dbg()) {
        fprintf(stderr, "[inst] arg0 tag=%d\n", JS_VALUE_GET_TAG(argv[0]));
    }

    /* WebAssembly.instantiate(bytes, imports) -> { module, instance } */
    {
        JSValue mod = wasm_module_new(ctx, argv[0]);
        JSValue ret;

        if (JS_IsException(mod))
            return mod;
        inst = wasm_instantiate_module(ctx, mod, imports);
        if (JS_IsException(inst)) {
            JS_FreeValue(ctx, mod);
            return inst;
        }
        ret = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ret, "module", mod);
        JS_SetPropertyStr(ctx, ret, "instance", inst);
        return ret;
    }
}

/* `new WebAssembly.Instance(module, imports)`.
 *
 * Vite's embedded hash modules are loaded exactly this way, so it is not
 * optional: without it the loader dies with "not a function" before any wasm
 * runs. The spec's constructor is the same operation as instantiate(module). */
static JSValue js_wasm_instance_ctor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue imports = argc > 1 ? argv[1] : JS_UNDEFINED;

    (void)new_target;
    return wasm_instantiate_module(ctx, argv[0], imports);
}

/* `new WebAssembly.Module(bytes)` */
static JSValue js_wasm_module_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc;
    return wasm_module_new(ctx, argv[0]);
}

/* `WebAssembly.compile(bytes)` -> Module.
 *
 * NOTE: the spec returns a Promise here, and real loaders rely on it - vite
 * does `WebAssembly.compile(bytes).then(WebAssembly.instantiate)`. This binding
 * is synchronous, which is what the embedder-facing tests and WhaleNode's
 * integration were written against, so making it async is a contract change
 * rather than a bug fix. It is the one remaining gap for vite-style wasm
 * loading; see the wasm section of doc-internal/architecture.md. */
static JSValue js_wasm_compile(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    if (whale_wasm_dbg())
        fprintf(stderr, "[compile] enter\n");
    return wasm_module_new(ctx, argv[0]);
}

/* ------------------------------------------------------------------ */
/* Module introspection: exports / imports / customSections            */
/* ------------------------------------------------------------------ */

/* Parse the export section into [{name, kind, index}]. `kind` uses the spec's
   numbering: 0 function, 1 table, 2 memory, 3 global. Kept separate from
   wasm_build_exports, which builds the live Instance surface; this is the
   static view a bundler reads without instantiating anything. */
static JSValue wasm_module_exports(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    WasmModule *m;
    size_t len = 0;
    uint8_t *buf;
    WasmReader r;
    JSValue out;

    /* Static method: the module is argv[0], not `this`. */
    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Module.exports: argument required");
    m = JS_GetOpaque(argv[0], wasm_classes_get(JS_GetRuntime(ctx))->module);
    if (!m)
        return JS_ThrowTypeError(ctx, "not a WebAssembly.Module");
    buf = JS_GetArrayBuffer(ctx, &len, m->bytes);
    if (!buf || len < 8)
        return JS_NewArray(ctx);
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    r.p = buf + 8;
    r.end = buf + len;
    r.ok = 1;
    while (r.p < r.end && r.ok) {
        uint8_t id = *r.p++;
        uint32_t size = wasm_uleb(&r);
        const uint8_t *sec;

        if (!r.ok || (size_t)(r.end - r.p) < size)
            break;
        sec = r.p;
        r.p += size;
        if (id == 7) {
            WasmReader er;
            uint32_t n, k;

            er.p = sec;
            er.end = sec + size;
            er.ok = 1;
            n = wasm_uleb(&er);
            for (k = 0; k < n && er.ok; k++) {
                uint32_t nlen = wasm_uleb(&er);
                JSValue e, nm;

                if (!er.ok || (size_t)(er.end - er.p) < nlen)
                    break;
                nm = JS_NewStringLen(ctx, (const char *)er.p, nlen);
                er.p += nlen;
                e = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, e, "name", nm);
                JS_SetPropertyStr(ctx, e, "kind",
                    JS_NewInt32(ctx, (int)*er.p++));
                JS_SetPropertyStr(ctx, e, "index",
                    JS_NewInt32(ctx, (int)wasm_uleb(&er)));
                JS_SetPropertyUint32(ctx, out, k, e);
            }
            break;
        }
    }
    return out;
}

/* Parse the import section into [{module, name, kind}]. */
static JSValue wasm_module_imports(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    WasmModule *m;
    size_t len = 0;
    uint8_t *buf;
    WasmReader r;
    JSValue out;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Module.imports: argument required");
    m = JS_GetOpaque(argv[0], wasm_classes_get(JS_GetRuntime(ctx))->module);
    if (!m)
        return JS_ThrowTypeError(ctx, "not a WebAssembly.Module");
    buf = JS_GetArrayBuffer(ctx, &len, m->bytes);
    if (!buf || len < 8)
        return JS_NewArray(ctx);
    out = JS_NewArray(ctx);
    if (JS_IsException(out))
        return out;
    r.p = buf + 8;
    r.end = buf + len;
    r.ok = 1;
    while (r.p < r.end && r.ok) {
        uint8_t id = *r.p++;
        uint32_t size = wasm_uleb(&r);
        const uint8_t *sec;

        if (!r.ok || (size_t)(r.end - r.p) < size)
            break;
        sec = r.p;
        r.p += size;
        if (id == 2) {
            WasmReader ir;
            uint32_t n, k;

            ir.p = sec;
            ir.end = sec + size;
            ir.ok = 1;
            n = wasm_uleb(&ir);
            for (k = 0; k < n && ir.ok; k++) {
                uint32_t mlen = wasm_uleb(&ir);
                JSValue e, mn;
                uint32_t flen;
                uint8_t kind;

                if (!ir.ok || (size_t)(ir.end - ir.p) < mlen)
                    break;
                mn = JS_NewStringLen(ctx, (const char *)ir.p, mlen);
                ir.p += mlen;
                flen = wasm_uleb(&ir);
                if (!ir.ok || (size_t)(ir.end - ir.p) < flen) {
                    JS_FreeValue(ctx, mn);
                    break;
                }
                e = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, e, "module", mn);
                JS_SetPropertyStr(ctx, e, "name",
                    JS_NewStringLen(ctx, (const char *)ir.p, flen));
                ir.p += flen;
                kind = *ir.p++;
                JS_SetPropertyStr(ctx, e, "kind", JS_NewInt32(ctx, (int)kind));
                /* skip the descriptor so the next entry starts in the right
                   place - it is what the reader would otherwise trip on */
                if (kind == 0) {
                    (void)wasm_uleb(&ir);
                } else if (kind == 1) {
                    (void)ir.p++;               /* elem type */
                    if (wasm_uleb(&ir) & 1)
                        (void)wasm_uleb(&ir);
                } else if (kind == 2) {
                    if (wasm_uleb(&ir) & 1)
                        (void)wasm_uleb(&ir);
                } else if (kind == 3) {
                    ir.p += 2;                  /* valtype, mutability */
                }
                JS_SetPropertyUint32(ctx, out, k, e);
            }
            break;
        }
    }
    return out;
}

/* customSections(module, name) -> array of Uint8Array payloads. Only the
   custom sections (id 0) whose name matches are returned, which is what a
   toolchain looks for when it reads producer metadata. */
static JSValue wasm_module_custom_sections(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    WasmModule *m;
    size_t len = 0;
    uint8_t *buf;
    const char *want;
    size_t want_len = 0;
    WasmReader r;
    JSValue out;
    uint32_t idx = 0;

    (void)this_val;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Module.customSections: arguments required");
    m = JS_GetOpaque(argv[0], wasm_classes_get(JS_GetRuntime(ctx))->module);
    if (!m)
        return JS_ThrowTypeError(ctx, "not a WebAssembly.Module");
    want = JS_ToCStringLen(ctx, &want_len, argv[1]);
    if (!want)
        return JS_EXCEPTION;
    buf = JS_GetArrayBuffer(ctx, &len, m->bytes);
    out = JS_NewArray(ctx);
    if (!buf || len < 8 || JS_IsException(out)) {
        if (want)
            JS_FreeCString(ctx, want);
        return out;
    }
    r.p = buf + 8;
    r.end = buf + len;
    r.ok = 1;
    while (r.p < r.end && r.ok) {
        uint8_t id = *r.p++;
        uint32_t size = wasm_uleb(&r);
        const uint8_t *sec;

        if (!r.ok || (size_t)(r.end - r.p) < size)
            break;
        sec = r.p;
        r.p += size;
        if (id == 0) {
            WasmReader cr;
            uint32_t nlen;

            cr.p = sec;
            cr.end = sec + size;
            cr.ok = 1;
            nlen = wasm_uleb(&cr);
            if (cr.ok && (size_t)(cr.end - cr.p) >= nlen &&
                nlen == want_len && (want_len == 0 || !memcmp(cr.p, want, nlen))) {
                JS_SetPropertyUint32(ctx, out, idx++,
                    JS_NewUint8ArrayCopy(ctx, cr.p + nlen,
                                         (size_t)(cr.end - (cr.p + nlen))));
            }
        }
    }
    if (want)
        JS_FreeCString(ctx, want);
    return out;
}

/* ------------------------------------------------------------------ */
/* install                                                             */
/* ------------------------------------------------------------------ */
int JS_WhaleInstallWasmRuntime(JSRuntime *rt)
{
    WasmClasses *cls;
    static const JSClassDef module_def = {
        "WebAssembly.Module", .finalizer = js_wasm_module_finalizer,
    };
    static const JSClassDef instance_def = {
        "WebAssembly.Instance", .finalizer = js_wasm_instance_finalizer,
    };
    static const JSClassDef memory_def = {
        "WebAssembly.Memory", .finalizer = js_wasm_memory_finalizer,
    };
    static const JSClassDef global_def = {
        "WebAssembly.Global", .finalizer = js_wasm_global_finalizer,
    };

    if (!rt)
        return -1;
    cls = wasm_classes_get(rt);
    if (!cls)
        return -1;
    /* JS_NewClass refuses an id that is already registered, which is exactly
       what a second call gets; nothing to do then */
    if (!JS_IsRegisteredClass(rt, cls->module))
        JS_NewClass(rt, cls->module, &module_def);
    if (!JS_IsRegisteredClass(rt, cls->instance))
        JS_NewClass(rt, cls->instance, &instance_def);
    if (!JS_IsRegisteredClass(rt, cls->memory))
        JS_NewClass(rt, cls->memory, &memory_def);
    if (!JS_IsRegisteredClass(rt, cls->global))
        JS_NewClass(rt, cls->global, &global_def);
    return 0;
}

int JS_WhaleInstallWasm(JSContext *ctx)
{
    JSValue global, wasm, ctor, proto;

    if (!ctx)
        return -1;
    if (JS_WhaleInstallWasmRuntime(JS_GetRuntime(ctx)) < 0)
        return -1;

    global = JS_GetGlobalObject(ctx);
    wasm = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, wasm, "validate",
        JS_NewCFunction(ctx, js_wasm_validate, "validate", 1));
    JS_SetPropertyStr(ctx, wasm, "compile",
        JS_NewCFunction(ctx, js_wasm_compile, "compile", 1));
    JS_SetPropertyStr(ctx, wasm, "instantiate",
        JS_NewCFunction(ctx, js_wasm_instantiate, "instantiate", 2));
    JS_SetPropertyStr(ctx, wasm, "compileStreaming",
        JS_NewCFunction(ctx, js_wasm_compile_streaming, "compileStreaming", 1));
    JS_SetPropertyStr(ctx, wasm, "instantiateStreaming",
        JS_NewCFunction(ctx, js_wasm_instantiate_streaming,
                        "instantiateStreaming", 2));

    /* Both constructors get a real prototype object. Without one the function
       has no `prototype` property, and `x instanceof WebAssembly.Module` -
       which esbuild's argument checker does on every call - throws instead of
       returning false. The class prototype is set to the same object, so the
       instances this file creates also answer instanceof correctly. */
    ctor = JS_NewCFunction2(ctx, (JSCFunction *)js_wasm_module_ctor,
                            "Module", 1, JS_CFUNC_constructor_or_func, 0);
    proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor));
    JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));
    /* The spec puts these on the constructor, not the prototype:
       WebAssembly.Module.exports(m) / .imports(m) / .customSections(m, name). */
    JS_SetPropertyStr(ctx, ctor, "exports",
        JS_NewCFunction(ctx, wasm_module_exports, "exports", 1));
    JS_SetPropertyStr(ctx, ctor, "imports",
        JS_NewCFunction(ctx, wasm_module_imports, "imports", 1));
    JS_SetPropertyStr(ctx, ctor, "customSections",
        JS_NewCFunction(ctx, wasm_module_custom_sections, "customSections", 2));
    JS_SetPropertyStr(ctx, wasm, "Module", ctor);
    JS_SetClassProto(ctx, wasm_classes_get(JS_GetRuntime(ctx))->module,
                      JS_DupValue(ctx, proto));
    JS_FreeValue(ctx, proto);

    ctor = JS_NewCFunction2(ctx, (JSCFunction *)js_wasm_instance_ctor,
                            "Instance", 1, JS_CFUNC_constructor_or_func, 0);
    proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "constructor", JS_DupValue(ctx, ctor));
    JS_SetPropertyStr(ctx, ctor, "prototype", JS_DupValue(ctx, proto));
    JS_SetPropertyStr(ctx, wasm, "Instance", ctor);
    JS_SetClassProto(ctx, wasm_classes_get(JS_GetRuntime(ctx))->instance,
                      JS_DupValue(ctx, proto));
    JS_FreeValue(ctx, proto);

    JS_SetPropertyStr(ctx, global, "WebAssembly", wasm);
    JS_FreeValue(ctx, global);
    return 0;
}


