# <1>: 环境与第三方源码获取
TIPS

## 工具链（详见 temp/environment.md）
xmake 3.0.9、gcc 16.1.0 (MinGW-w64, x86_64-win32-seh)、cmake 4.4.0-rc2、mingw32-make、python 3.12、
node 24。缺失：MSVC/clang/ninja/tcc/docker/cargo。本机 shell 是 PowerShell（不是 bash）。

## 源码获取（踩坑记录）
- `git clone https://github.com/quickjs-ng/quickjs` 在 PowerShell 里连 github:443 超时（curl 28）。
  可用替代：`https://codeload.github.com/<owner>/<repo>/zip/refs/heads/<branch>` 直接下 zip 再 Expand-Archive。
- TinyCC 官网 https://bellard.org/tcc/ 只给文档链接；实际二进制/源码在
  https://download.savannah.nongnu.org/releases/tinycc/ ，最新源码是 **0.9.27**（没有 0.9.28）。
- 目录约定：`.gitignore` 已排除 temp/ 与 build/；第三方源码最终落 thirdparty/（见 <2>）。

## baseline 验证
原版 QuickJS-ng 0.16.2 用 cmake + MinGW Makefiles 编译通过，`qjs.exe -e "console.log(...)"` 输出正常，
`--version` 报 0.16.2。命令见 temp/qjs-build（cmake 生成，产物在 temp/ 内，不入 git）。

# <2>: 目录结构：src/ + thirdparty/，xmake 平台名是 mingw
TIPS

- QuickJS-ng 源码从 temp/ 迁到 **src/**（我们的改造对象）；TinyCC 放 **thirdparty/tinycc/**。
  temp/ 只留下载物与 temp/environment.md。
- **xmake 平台坑**：本机 xmake 平台名是 `mingw`，不是 `windows`，`is_plat("windows")` 为假。
  xmake.lua 第一版因此在 Windows 上走了非 Windows 分支 → `add_syslinks("dl")` → 链接报
  `ld.exe: cannot find -ldl`。修法：`is_win() = is_plat("windows") or is_plat("mingw")`，
  链接参数对齐 quickjs-ng cmake 的 MINGW 分支（`-static -static-libgcc -Wl,--stack,8388608 -lpthread`）。
- 构建产物路径：`build/mingw/x86_64/release/qjs.exe`（平台名同样体现为 mingw）。
- **构建已通过**：`xmake f -y -p mingw -m release` + `xmake -y` 成功；`qjs.exe -e` 输出正常。
  不带 `-p mingw` 时 `xmake f` 会报 `target(qjs): toolchain not found!`（本机没装 VS）。
- 性能基线（解释器，JIT 前的对照）：`fib(30) = 832040`，**253ms**。
- src/ 精简：移除 test262/、.github/、docs/、run-test262.c、fuzz.c、qjs-wasi-reactor.c、
  meson.build/meson_options.txt、unicode_download.sh、update-version.sh、.gitmodules；src 现 3.5MB。

# <3>: 工具使用坑：对源码/构建文件落笔后，本回合 bash 会被拒
TIPS

- 宿主规则：一旦本回合用 file tool（write_file/edit_file）改过 `xmake.lua` 或源文件（如 `src/quickjs.c`），
  本回合剩下来的 **bash 调用全部被拒**（提示 `bash cannot declare which files it changes while a
  read-evidence requirement is outstanding: <path>`），连只读命令也拒；**下一个用户回合自动恢复**。
  → 对策：验证性命令（编译、跑测试、git commit）安排在同一回合的**前半段**执行，文件落笔放后半段；
  或者干脆下回合再验证。
- bash 无法直接把多行内容写进源文件（会被上面这条拦），改源码只能走 `edit_file`，且**每次编辑前要重新
  read 目标窗口**：同一回合内对同一文件连续两次 edit 会报 `file changed after read`/`missing lines a-b`。
  实践节奏：read 窗口 → edit 一处 → read 窗口 → edit 下一处。

# <4>: Step 3 —— whalejs 接口与单元测试
SOLVED

- 接口 `src/whalejs.h`（新增，只加不改上游 `quickjs.h`）：`JSJitMode`(OFF/LAZY/AOT)、`JSWhaleJitStats`、
  `JSWhaleContext`（内嵌状态，零分配）、`JS_WhaleSetJitMode/GetJitMode/GetJitStats`，以及引擎内部
  `JS_WhaleNoteJitRequest/NoteJitResult`。新建 context 默认 `JS_JIT_LAZY`。
- `src/quickjs.c` 侵入点只有 4 处（上游升级时重放这 4 处即可）：include `whalejs.h`；
  `struct JSContext` 末尾加 `JSWhaleContext whale;`；`JS_NewContext` 里设默认模式；
  `JS_SetContextOpaque` 之后追加实现块。
- 测试：`tests/whale/test_api.c`（无框架 CHECK → exit code）、`tests/whale/fib.js`（fib(30) 语义+耗时）。
  构建目标 `whale_test`，跑法 `xmake run whale_test`。
- 验证（commit 22c3a46）：`xmake -y` build ok；`xmake run whale_test` → `whale api test: ok`；
  `qjs tests/whale/fib.js` → `fib(30)=832040 244ms`（仍是解释器，与 baseline 253ms 同级）。
- 待优化：xmake 里 `qjs` 与 `whale_test` 各编一份引擎源码，全量构建 ~75s；target 变多后抽
  `whalejs_lib` 静态库共享。

# <5>: Step 4 —— JIT 挂接层（热点统计）
SOLVED

- `src/whalejs.h`：加 `WHALE_JIT_HOT_THRESHOLD`（默认 100，可用 `-D` 覆盖）。
- `JSFunctionBytecode` 末尾加两个字段：`uint32_t whale_call_count`、`void *whale_jit_entry`。
  **零初始化已查证**：Bytecode 只有两处分配，均为 `b = js_mallocz(ctx, function_size)`——
  编译路径 37108、反序列化 `JS_ReadFunctionBytecode` 路径 39812，所以字段天生为 0。
  （`function_size = sizeof(*b)`，新字段自动计入分配大小。）
- 挂接点：`JS_CallInternal` 里 `ctx = b->realm;` 之后调 `whale_on_call(ctx, b)`。
  协程恢复走 `goto restart`，会跳过这个 hook（第一版可接受）。
- `whale_on_call()`（static，定义在实现块里，早于 18240 的调用点）：`JS_JIT_OFF` 立即 return（零开销）；
  否则 `stat_requests++`、`whale_call_count++`；达阈值且尚无 `whale_jit_entry` 时进 hot 分支——
  该分支目前是空的，下一步在这里调 TinyCC 产出机器码。
- 测试新增两条：JIT=OFF 评估三段 JS 后 `requests` 纹丝不动；JIT=LAZY 评估后 `requests` 增长且结果与 OFF 一致。
- 验证（commit bdd65de）：build ok 69.5s；`whale api test: ok`；`fib(30)=832040 236ms`（挂计数后与 244ms 同级，
  说明 hook 开销可忽略）；`fib(25)=75025`。
- 下一步：在 MinGW 下构建 libtcc（thirdparty/tinycc），把 hot 分支换成真实编译。


