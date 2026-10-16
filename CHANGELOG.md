# WhaleJS Changelog

WhaleJS 自己的版本（MAJOR.MINOR.PATCH），与 QuickJS-ng 的版本解耦：
上游升级时只合并它的代码，绝不跟着它改这里的号。
`whalejs --version` 同时打出两个版本。

规则：MAJOR = 对外接口 / JIT ABI 不兼容；MINOR = 新优化 / 新 opcode
覆盖；PATCH = 纯修 bug。

## 0.2.0

WhaleJS 变成可分发的库：编译产物含静态库、动态库与按惯例打包的归档。

- 构建：`libwhalejs.a`（静态）+ `whalejs.dll`/`libwhalejs.so`（动态，Windows 另有
  导入库 `whalejs.dll.a`）；CLI 改名 `whalejs_cli`（产物仍是 `whalejs.exe`）；
  还有 `whale_test`（单元测试）与 `whale_demo`（嵌入示例，只依赖公开头文件+库）
- 新增 `xmake whale_pack`：把 include/（quickjs.h、quickjs-libc.h、whalejs.h、
  LICENSE）与 lib/（静态库 + 动态库）打成 `.zip`，**不含 README、不含 demo exe、
  不含任何 tcc 相关文件**；zip 用纯 Lua 写（store 法），不依赖外部 tar/zip
- **JIT 运行期零外部文件**：生成的机器码只引用宿主符号——`JS_Whale*`/
  `whale_jit_*` 包装、少量 libc/libm（memset/fmod/...），以及 TinyCC 运行库里的
  64 位转换辅助（`__floatundidf`/`__fixunsdfdi`/...，直接从 `src/tcc/lib/libtcc1.c`
  编进库里）。`JS_WhaleTccNoStdLibs()` 让 TCC 跳过 CRT 链接，于是
  **不需要 libtcc1-64.a，也不需要任何 .def 文件**。使用者拿 `libwhalejs.a`
  （或 DLL）+ 头文件直接链接即可，磁盘上没有任何 tcc 运行期文件
- 新增 API `JS_WhaleSetJitRuntimeDir()`：JIT 目录探测（本库所在目录相对位置 →
  环境变量 `WHALE_JIT_RUNTIME_DIR` → 构建期路径）；现在它只是搜索路径，包解压到
  任意位置都能用
- 移除 `--libtcc_prebuilt` 选项（预编译 libtcc.dll 的开发捷径）；TinyCC 一律从源码编
- CI：`.github/workflows/build.yml` 在 Windows/Linux/macOS 三端构建库+CLI、
  跑单元测试与 perftest、出归档

## 0.1.0

第一个可用版本：TinyCC JIT 跑通，perftest 25/25 全绿（0 mismatch）。

- 数值特化（double 域 + guard + deopt 整帧重跑）与表达式融合
- 比较-分支融合、位运算短式（ToInt32/`%`/`!x`）、热局部写点物化
- 小 trip 循环展开（minmax 追平 node）、Uint8Array fill/count 整循环消除
- 对象：CStruct 列存储、free list、原地 fusion（immutable 进 100ms 线）
- 后端：TCC 单例多链接、gcc 按函数二级编译、ISA 分级框架（CPUID+覆盖）
