# wasm3（vendored）

WhaleJS 的 WebAssembly 扩展用它做执行引擎。绑定层在 `src/whale_wasm.c`，
构建目标是 `WhaleJS-wasm`（xmake.lua）。

- 上游：https://github.com/wasm3/wasm3
- 版本：0.9.2（commit `28ecb9af6d2040e474a70f7cb7f43666740141fb`）
- 许可证：MIT，见 `LICENSE`
- 取的是 `source/` 目录，只保留核心解释器需要的文件：

  | 保留 | 说明 |
  |------|------|
  | `m3_bind.c` `m3_code.c` `m3_compile.c` `m3_core.c` `m3_env.c` `m3_exec.c` | 解释器本体 |
  | `m3_function.c` `m3_info.c` `m3_module.c` `m3_parse.c` | 模块解析与函数表 |
  | `m3_validate.c` `m3_xxh64.c` `m3_deterministic.c` | 校验与辅助 |
  | `wasm3.h` 等头文件 | 公开 API |

  删掉了 `m3_api_wasi*.c` / `m3_api_uvwasi.c` / `m3_api_libc.c` /
  `m3_api_meta_wasi.c` / `m3_api_tracer.c` / `m3_snapshot.c` 以及
  `extensions/`、`extra/`：WASI 需要额外依赖，而且宿主 API 应该由嵌入方
  通过 `imports` 提供，不该固化在库里。

## 编译期开关

`xmake.lua` 里显式关掉了几个提案，只留 MVP + 常用扩展：

```
d_m3HasTypedRefs=0  d_m3HasStackSwitching=0  d_m3HasSnapshots=0
d_m3HasExceptionHandling=0  d_m3HasMemory64=0
```

打开 `d_m3HasTypedRefs` 会连带打开 stack switching / snapshots（见
`m3_config.h`），体积和复杂度都上一个台阶，当前用不到。

## 升级时注意

- `m3_GetMemory` 的签名在 0.5 → 0.9 之间从 `(runtime, ...)` 改成了
  `(module, ...)`；`m3_FindFunctionIn` 也是 0.9 才有的。绑定层用的是新签名。
- 导出表没有公开的遍历 API，`src/whale_wasm.c` 自己扫 export section
  （只读字节，不碰 wasm3 内部结构）。
