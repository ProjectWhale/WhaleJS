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

/* JIT 后端查找运行期数据的目录。发布版**不需要任何 tcc 运行期文件**（生成的
   机器码只引用宿主符号，CRT 不链接），所以这只是一个搜索路径，给"用户自己保留
   了一份 tcc-runtime"的场景用。默认顺序：本库所在目录的相对位置
   （<dir>/tcc-runtime、<dir>/lib/tcc-runtime、<dir>/../lib/tcc-runtime）→
   环境变量 WHALE_JIT_RUNTIME_DIR → 构建期路径。
   dir：含 lib/libtcc1-64.a 的目录；NULL 或空串恢复自动探测。 */
JS_EXTERN void JS_WhaleSetJitRuntimeDir(const char *dir);

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

#ifdef __cplusplus
}
#endif

#endif /* WHALEJS_H */
