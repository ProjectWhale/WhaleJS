/*
 * WhaleJS —— JIT/AOT 扩展的对外接口
 * 只加不改：上游 quickjs.h 保持原样，新能力全部收在本头文件里。
 */
#ifndef WHALEJS_H
#define WHALEJS_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JIT 模式 */
typedef enum {
    JS_JIT_OFF = 0,   /* 纯解释器，等价原版 */
    JS_JIT_LAZY = 1,  /* 热点函数按需编译（默认） */
    JS_JIT_AOT = 2,   /* 另允许空闲时 AOT 纯 JS 函数 */
} JSJitMode;

/* JIT 统计（调试与测试用） */
typedef struct JSWhaleJitStats {
    uint32_t requests;   /* 进入 JIT 候选的次数 */
    uint32_t compiled;   /* 编译成功数 */
    uint32_t failed;     /* 编译失败数（已回退解释器） */
    uint64_t code_bytes; /* 生成的机器码字节数 */
    uint32_t deopt_shape; /* deopt census：字段 resolve 失败（shape 对不上） */
    uint32_t deopt_tag;   /* deopt census：字段 tag 非数值 */
    uint32_t deopt_guard; /* deopt census：其余一切（入口/类型/比较守卫） */
} JSWhaleJitStats;

/* 引擎内部状态：内嵌在 JSContext 里，无单独分配、无需释放 */
typedef struct JSWhaleContext {
    int      jit_mode;
    uint32_t stat_requests;
    uint32_t stat_compiled;
    uint32_t stat_failed;
    uint64_t stat_code_bytes;
    uint32_t stat_deopt_shape;
    uint32_t stat_deopt_tag;
    uint32_t stat_deopt_guard;
} JSWhaleContext;

/* 热点阈值：函数累计调用达到该次数即进入 JIT 候选 */
#ifndef WHALE_JIT_HOT_THRESHOLD
#define WHALE_JIT_HOT_THRESHOLD 100
#endif

/* Profiling window. A function is interpreted WHALE_JIT_HOT_THRESHOLD times before
   it is compiled, and only the last WHALE_PROFILE_CALLS of those are recorded: the
   first calls are still warming up, and measuring only the tail keeps the cost where
   it is worth paying. A call that repeats its arguments teaches nothing new, so the
   memo check skips those on its own. */
#ifndef WHALE_PROFILE_CALLS
#define WHALE_PROFILE_CALLS 8
#endif
/* Bounded on purpose - a script reaches tens of thousands of objects, so an access
   site remembers at most WHALE_PROFILE_SHAPES shapes and a function at most
   WHALE_PROFILE_SITES sites. Overflow makes the generator fall back to the general
   form, which is what it does today. */
#ifndef WHALE_PROFILE_SITES
/* 64 rather than 16: the table is direct-mapped on (offset >> 2), and a collision
   disables BOTH sites involved, so a small table turns ordinary programs into
   "polymorphic" ones. object alone has nine access sites and two of them collided in
   a table of 16 (offset 29 and offset 93 both landed in slot 7), which made a stable
   object look unstable. The table is only touched during the last few interpreted
   calls, so the extra memory buys correctness cheaply. */
#define WHALE_PROFILE_SITES 64
#endif
#ifndef WHALE_PROFILE_SHAPES
#define WHALE_PROFILE_SHAPES 4
#endif

/* WhaleJS 自己的版本（MAJOR.MINOR.PATCH），与 QuickJS-ng 的版本解耦：
   上游升级时只合并它的代码和它的 QJS_VERSION_*，绝不跟着它改这里。
   MAJOR = 对外接口 / JIT ABI 不兼容；MINOR = 新优化 / 新 opcode 覆盖；
   PATCH = 纯修 bug。见 CHANGELOG.md。 */
#define WHALE_VERSION_MAJOR 0
#define WHALE_VERSION_MINOR 2
#define WHALE_VERSION_PATCH 0

/* WhaleJS 版本字符串（"0.1.0" 形），由上面的三元组合成。 */
JS_EXTERN const char *JS_WhaleGetVersion(void);

/* 公开 API */
JS_EXTERN void      JS_WhaleSetJitMode(JSContext *ctx, JSJitMode mode);
JS_EXTERN JSJitMode JS_WhaleGetJitMode(JSContext *ctx);
JS_EXTERN void      JS_WhaleGetJitStats(JSContext *ctx, JSWhaleJitStats *out);

/* 选择 JIT 后端的编译器驱动 —— 这是嵌入方的接口，不是环境变量：本引擎是基础
   支持库，gcc/clang 装在哪里只有嵌入它的人知道。
   driver：编译器可执行文件（建议完整路径，如 "C:/mingw64/bin/gcc.exe"）；
           NULL 或空串恢复默认（TinyCC）。
   flags：  传给驱动的额外参数（可为 NULL）；TinyCC 后端忽略。
   两者内部各保留一份拷贝，可随时再次调用覆盖；须在本进程第一次 JIT 编译前设置。 */
JS_EXTERN void JS_WhaleSetJitCc(const char *driver, const char *flags);

/* Compile one function with the gcc backend instead of TinyCC. The function
   keeps running interpreted until it goes hot; at that point the gcc backend
   compiles it (`-c`, linked in-process by TinyCC itself - no shared object,
   no LoadLibrary) exactly where the TinyCC backend would have. Needs a driver
   named with JS_WhaleSetJitCc, otherwise the mark has no effect; every
   function not so marked stays on TinyCC, so gcc is opt-in per function. */
JS_EXTERN void JS_WhaleSetJitCcForFunction(JSContext *ctx, JSValue func);

/* 引擎内部：JIT 路径用于累加统计（host 一般不用） */
JS_EXTERN void JS_WhaleNoteJitRequest(JSContext *ctx);
JS_EXTERN void JS_WhaleNoteJitResult(JSContext *ctx, int ok, uint64_t code_bytes);

/* 跑一次嵌入方注册的中断处理函数（JS_SetInterruptHandler），返回 1 表示
   要求停止执行。给"在引擎之外自己跑机器码/解释器"的扩展用：这类代码不在
   JS_CallInternal 里，碰不到 js_poll_interrupts()，而 handler 本身藏在私有
   的 JSRuntime 里。返回 1 时异常已经挂好（和解释器中断时抛的是同一个
   "interrupted"，且不可 catch），调用方只需要把控制流退回引擎，不要再抛
   第二个异常。 */
JS_EXTERN int JS_WhalePollInterrupt(JSContext *ctx);

/* ---- WebAssembly 扩展（可选组件） ----------------------------------------
   实现在单独的静态库里（xmake target WhaleJS-wasm，基于 wasm3），引擎本体
   不依赖它：不链接 WhaleJS-wasm 时下面这些符号不存在，引擎行为完全不变。

   挂载是两步：
     1. JS_WhaleInstallWasmRuntime(rt)：在 runtime 上注册 Module / Instance /
        Memory 三个自定义类。同一个 runtime 多次调用是安全的（幂等）。
     2. JS_WhaleInstallWasm(ctx)：在上面基础上给这个 context 定义
        globalThis.WebAssembly。它内部会先做第 1 步。

   提供的 API（同步，不返回 Promise）：
     WebAssembly.validate(bytes)                -> boolean
     WebAssembly.compile(bytes)                 -> WebAssembly.Module
     WebAssembly.instantiate(module, imports)   -> WebAssembly.Instance
     WebAssembly.instantiate(bytes, imports)    -> WebAssembly.Instance
   bytes 接受 ArrayBuffer / Uint8Array；imports 形如
   { 模块名: { 导入名: function } }。Instance.exports 上是导出的函数和
   memory（一个带 .buffer getter 的对象，buffer 是 wasm 线性内存上的零拷贝
   ArrayBuffer，memory.grow 之后旧 buffer 会被 detach）。

   返回 0 成功，-1 失败（JS 异常已挂起）。 */
JS_EXTERN int JS_WhaleInstallWasmRuntime(JSRuntime *rt);
JS_EXTERN int JS_WhaleInstallWasm(JSContext *ctx);

/* ---- 复用 JIT 后端（给扩展用） -------------------------------------------
   把一段 C 函数体编译进引擎**唯一**的那个 TCCState，取回符号地址。WebAssembly
   扩展用它把 wasm 翻译成 C 之后编译。

   引擎的 TCCState 是进程级单例：tcc_new() 在 state 已存在时直接返回它，
   tcc_delete() 对它是空操作。扩展仍应把 token 交给这里，而不是自己驱动
   parser——这个入口会做编译单元的开合、relocate 的时机和宿主符号的解析。

   emit：parser 已经就绪时被调用，负责用 JS_WhaleTok* 推一个完整 C 函数。
   symbol：要取回的符号名（调用方保证唯一）。
   返回 1 成功，*fn_out 是符号地址；失败 0，*fn_out 为 NULL。

   JS_WhaleJitReady()：确保 state 已建立，返回 1 表示可用。扩展在编译前调一次。 */
JS_EXTERN int JS_WhaleCompileExtern(void (*emit)(void *), void *emit_ctx,
                                    const char *symbol, void **fn_out);
JS_EXTERN int JS_WhaleJitReady(void);

/* 批量编译的收尾：relocate 一次，之后用 JS_WhaleFindSymbol 取地址。
   为什么必须批量：relocate 会把整个 state 的代码重排，之前取到的函数指针
   在下次 relocate 后就失效了（实测调用会崩）。所以 N 个
   JS_WhaleCompileExtern 之后只调一次 JS_WhaleRelocateExtern，地址在它之后
   再取。 */
JS_EXTERN int JS_WhaleRelocateExtern(void);
JS_EXTERN void *JS_WhaleFindSymbol(const char *symbol);

/* 每次 relocate 递增的代号。JS_WhaleFindSymbol 取到的地址只在取它时的那个
   代号内有效：relocate 会把整个 state 的代码重排。缓存地址的扩展要把代号
   一起记下来，不一致时重新解析——不要试图维护"所有实例"的全局表去批量重绑，
   因为实例可能属于已经被销毁的 JSRuntime。 */
JS_EXTERN unsigned JS_WhaleJitGeneration(void);

/* 给经 JS_WhaleCompileExtern 编译的代码注册一个宿主符号（扩展自己的运行期
   辅助函数）。名字会被拷贝。注意：注册符号不等于声明——生成的 token 流里
   仍然要有原型，否则 TinyCC 会按 `int fn()` 处理并在返回类型上报错。 */
JS_EXTERN int JS_WhaleAddHostSymbol(const char *name, void *addr);

#ifdef __cplusplus
}
#endif

#endif /* WHALEJS_H */
