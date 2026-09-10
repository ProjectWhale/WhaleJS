# 对外接口契约 v0（草案）

原则：先定接口 → 写测试 → 再实现（doc/test driven）。上游 API 不动，新能力加到独立头文件。

## 1. 沿用的 QuickJS-ng API
`src/quickjs.h` 原样保留：`JS_NewRuntime` / `JS_NewContext` / `JS_Eval` / `JS_Call` / `JS_NewCFunction` /
`JS_SetInterruptHandler` / `JS_FreeValue` ...。不破坏上游 API/ABI，便于跟随上游版本。

## 2. JIT / AOT 控制（新增：`src/whalejs.h`，前缀 `JS_Whale*` 避免与上游撞名）
```c
typedef enum { JS_JIT_OFF = 0, JS_JIT_LAZY = 1, JS_JIT_AOT = 2 } JSJitMode;

void      JS_WhaleSetJitMode(JSContext *ctx, JSJitMode mode);
JSJitMode JS_WhaleGetJitMode(JSContext *ctx);
/* 统计：请求/成功/失败编译数、机器码字节数 —— debug 与测试用 */
void      JS_WhaleGetJitStats(JSContext *ctx, JSWhaleJitStats *out);
```
引擎内部接口（JIT 编译路径用，host 一般不用）：
`JS_WhaleNoteJitRequest(ctx)` / `JS_WhaleNoteJitResult(ctx, ok, code_bytes)` —— 累加统计。

契约：
- 新建 context 默认 `JS_JIT_LAZY`（JIT 实现前与解释器等价）。
- `JS_JIT_OFF`：行为与耗时特征都必须退化为原版解释器。
- 编译失败（TCC 报错、含不支持形态）**静默回退解释器**，不得抛 JS 异常、不得中断执行。
- 同一函数被 JIT 后，语义与解释器完全一致（含异常、`this`、`arguments`、闭包）。

## 3. AOT 工具（新增，基于 `qjsc.c` 改造为 `whalec`）
输入 JS，输出 C 数组：预编译机器码 + 字节码回退路径。产物在无 JIT 平台上必须能回退执行。

## 4. WhaleUI 对接（预留，Full 版本）
```c
/* 宿主能力表：DOM / 事件 / 媒体，由 WhaleUI 注册 */
void JS_WhaleSetHost(JSContext *ctx, const JSWhaleHost *host);
```
- JS 侧给出完整 DOM / Event / `HTMLMediaElement` 定义，但**不实现**，调用全部转 host。
- 未注册 host 时，这些 API 存在但调用报 `TypeError`（Lite 版本即此状态）。

## 5. 构建开关
```powershell
xmake f -y -p mingw -m release --jit=y --edition=lite   # 默认
```
- `--jit=n`：不编 JIT 代码，等价原版。
- `--edition=lite|full`：full 才链 thirdparty 的网络/TLS/wasm/媒体库。
