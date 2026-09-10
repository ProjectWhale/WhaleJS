The process of project: WhaleJS —— 基于 QuickJS-ng + TinyCC 实现带 JIT/AOT 的改造版 QuickJS 引擎（Full / Lite 两版）

<1>(TIPS): 环境与第三方源码获取 —— 工具链版本、github clone 超时的替代下载方式、TCC 0.9.27 版本确认、baseline 编译验证。见 current.md

<2>(TIPS): 目录结构定为 src/（改造源码）+ thirdparty/（TinyCC 等）；xmake 平台名是 mingw，`is_plat("windows")` 为假会导致误加 -ldl。见 current.md

<3>(TIPS): 工具坑 —— 本回合用 file tool 改过 xmake.lua/源码后，本回合余下 bash 全被拒（跨回合恢复）；改源码只能 edit_file 且每次编辑前要重读窗口。见 current.md

<4>(SOLVED): Step 3 —— whalejs 对外接口（src/whalejs.h）+ quickjs.c 4 处侵入点 + tests/whale 单元测试，已跑通（whale api test: ok，commit 22c3a46）。见 current.md

<5>(SOLVED): Step 4 —— JIT 挂接层：JS_CallInternal 调 whale_on_call 做热点计数，JSFunctionBytecode 加 whale_call_count/whale_jit_entry（js_mallocz 零初始化），hot 分支留待接 TinyCC（commit bdd65de）。见 current.md

<6>(SOLVED): Step 5 —— TinyCC 接入：config.h 手工落库（configure 需要 sh）、libtcc 单 TU 编译、tcc-runtime（include + libtcc1-64.a）落库、xmake 加 libtcc target；运行期编译已跑通（add/fib）。见 current.md

<7>(SOLVED): Step 6 前哨 —— JIT 目标代码 ABI 原型跑通（TCC 编的 C 能操作真实 JSValue，`jit fibi(10)=55` 与解释器 MATCH）；两个坑：ng 版公开头没有 JS_Add/JS_Lt、PowerShell 里 `-Wl,--stack,` 要加引号；含第一版 opcode 子集与解释器语义清单。见 current.md

<8>(SOLVED/UNRESOLVED 性能): Step 6 —— 第一版 JIT 跑通（字节码→C→TCC，原生 int 快路径 + 复用解释器 slow 路径；tests 断言 compiled>=1 且语义正确，commit 9581e82），但比解释器还慢（3000× fibi(30)：10ms vs 16ms）——TCC 不优化 + 值栈仍在内存。下一步优化路线（栈→寄存器提升、类型特化、递归展开、常量折叠、TCC 优化、编译缓存）已记入。见 current.md

<9>(UNRESOLVED 性能): Step 6 续 —— 栈槽提升为 C 局部变量（v0..vN，编译期跟踪 sp，跳转深度不一致就放弃）已入库（commit d019316），但没提速：原版解释器 2200 ns/call vs TCC JIT 3200 ns/call（慢 45%）。诊断：瓶颈不是栈访问，而是 TCC 不做优化、打不过 gcc -O3 的解释器；收益只能来自去装箱/类型特化（int 域内循环 + guard + 去优化），保底路线是 AOT 用 gcc -O3。见 current.md

<10>(UNRESOLVED 性能/大幅改善): Step 7 —— 数值特化（int + double 统一到 C double 域）已落地：抽象解释 + guard + deopt 重跑同一帧（commit b6d8569）。fibi(30) 从 3200 降到 1800 ns/call（原版解释器 1600），即从"慢 100%"追到"慢 12%"，仍未反超；根因是 TCC 无寄存器分配、代码质量差。下一步：寄存器形态生成（消栈槽）/ AOT gcc -O3 / 分支预测与 CPU 缓存优化（已记录，待实现）/ 支持 call 以覆盖递归 fib。见 current.md

<11>(SOLVED): Step 8 —— 表达式形态生成 + 参数解箱：两条 peephole（`load load binop [put_loc]`、`load put_loc`）消掉栈槽往返，参数入口一次性解箱，另修三个让生成器静默放弃的 bug，并加 `WHALE_JIT_DUMP=1` 转储生成源码（commit cd04d7f）。结果：fibi(30) **200 ns/call vs 解释器 1800**（9×），语义全对。下一步：opcode 层 DCE/不变式外提、支持 `call`（递归 fib）、预留 AOT 后端接口。见 current.md
