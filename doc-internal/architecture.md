# WhaleJS 架构（内部文档）

## 目标
用 QuickJS-ng + TinyCC 做一个支持 **JIT / AOT** 的 JS 引擎，与 WhaleUI 组成轻量浏览器引擎。
JIT 要求真编译成机器码（TinyCC 后端），不是把解释器循环包一层。

## 目录
| 目录 | 用途 |
|------|------|
| `src/` | 改造版 QuickJS 源码（起点 = QuickJS-ng master，0.16.2） |
| `thirdparty/` | 依赖源码：当前只有 `tinycc/`（0.9.27）；规划 libhv / mbedtls / reproc / wasm3 |
| `doc/` | 对外文档 |
| `doc-internal/` | 内部实现与接口文档（本目录） |
| `notes/` | 长期问题与经验，见 `notes/index.md` |
| `temp/` | 临时数据（下载物、baseline 构建目录），**不入 git** |

## 构建
```powershell
xmake f -y -p mingw -m release   # 必须带 -p mingw：本机没有 MSVC，否则报 "toolchain not found!"
xmake -y
.\build\mingw\x86_64\release\qjs.exe -e "console.log(1+2)"
```
- xmake 3.0.9；本机平台名是 **mingw**（不是 windows），`is_plat("windows")` 为 false，判平台用 `xmake.lua` 里的 `is_win()`。
- MinGW 链接参数对齐 quickjs-ng 的 cmake MINGW 分支：`-static -static-libgcc -Wl,--stack,8388608` + `-lpthread`。
  少了这些会报 `cannot find -ldl`（走错非 Windows 分支）或运行时缺 winpthread。
- 引擎本体是 C；自有新代码用 C++14，**禁模板与 try-catch**（rules/user.md）。

## 两个版本
- **Lite**：纯 JS —— 引擎 + 标准库 + JIT/AOT。不链网络/多媒体。
- **Full**：Lite + 浏览器向 API —— fetch/libhv、TLS/mbedtls、wasm/wasm3、多媒体（reproc 驱动 ffmpeg CLI，当前无法渲染，先留接口）。

## 与 WhaleUI 的边界
WhaleUI 负责 DOM 底层、事件、多媒体播放；WhaleJS 负责 JS 运行时。
- JS 侧提供完整 DOM / Event 定义，**不实现**，通过 host 函数表接入（Full 版本）。
- 媒体能力以 JS API 形态预留（`HTMLMediaElement` 等），底层调用转给 WhaleUI。

## 不变量
- 保持与上游 QuickJS-ng 的兼容：改动尽量收在 hook 点（如 `JS_CallInternal` 入口、`JSFunctionBytecode` 附加字段），便于后续跟上游。
- JIT 必须可编译期/运行期关闭，关闭后行为等同原版。
