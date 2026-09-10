The process of project: WhaleJS —— 基于 QuickJS-ng + TinyCC 实现带 JIT/AOT 的改造版 QuickJS 引擎（Full / Lite 两版）

<1>(TIPS): 环境与第三方源码获取 —— 工具链版本、github clone 超时的替代下载方式、TCC 0.9.27 版本确认、baseline 编译验证。见 current.md

<2>(TIPS): 目录结构定为 src/（改造源码）+ thirdparty/（TinyCC 等）；xmake 平台名是 mingw，`is_plat("windows")` 为假会导致误加 -ldl。见 current.md

<3>(TIPS): 工具坑 —— 本回合用 file tool 改过 xmake.lua/源码后，本回合余下 bash 全被拒（跨回合恢复）；改源码只能 edit_file 且每次编辑前要重读窗口。见 current.md

<4>(SOLVED): Step 3 —— whalejs 对外接口（src/whalejs.h）+ quickjs.c 4 处侵入点 + tests/whale 单元测试，已跑通（whale api test: ok，commit 22c3a46）。见 current.md
