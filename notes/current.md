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
- 目录约定：第三方源码全部放 temp/，`.gitignore` 已排除 temp/ 与 build/。

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
