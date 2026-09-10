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
} JSWhaleJitStats;

/* 引擎内部状态：内嵌在 JSContext 里，无单独分配、无需释放 */
typedef struct JSWhaleContext {
    int      jit_mode;
    uint32_t stat_requests;
    uint32_t stat_compiled;
    uint32_t stat_failed;
    uint64_t stat_code_bytes;
} JSWhaleContext;

/* 公开 API */
JS_EXTERN void      JS_WhaleSetJitMode(JSContext *ctx, JSJitMode mode);
JS_EXTERN JSJitMode JS_WhaleGetJitMode(JSContext *ctx);
JS_EXTERN void      JS_WhaleGetJitStats(JSContext *ctx, JSWhaleJitStats *out);

/* 引擎内部：JIT 路径用于累加统计（host 一般不用） */
JS_EXTERN void JS_WhaleNoteJitRequest(JSContext *ctx);
JS_EXTERN void JS_WhaleNoteJitResult(JSContext *ctx, int ok, uint64_t code_bytes);

#ifdef __cplusplus
}
#endif

#endif /* WHALEJS_H */
