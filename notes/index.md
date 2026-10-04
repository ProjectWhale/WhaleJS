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

<12>(SOLVED): Step 9 —— 自递归 fib 也能 JIT：`get_var_ref` 只接受"闭包槽就是本函数自己"（运行时校验，保证纯函数→deopt 重跑安全），`call` 直接调自己的 JIT 入口（不经引擎建帧），加栈溢出检查；修 3 个 bug（exc 标签丢失、强转少参数、is_self 读错 `sf->var_refs` 而非闭包 `var_refs`——这个让守卫永远失败、静默退回解释器）。结果：递归 fib(24)×30 353→293ms（1.2×），迭代 fibi 保持 200 ns/call（9×），语义全过。commit c03e345。见 current.md

<13>(SOLVED): Step 10 —— 常量传播 + 死代码消除（吃掉 temp/tcc_to_optimize.md 优先级 1，且不用动 TCC）：只写一次的常量局部变量在读取处替换成字面量（`push_const` 数值常量也参与折叠），return/goto 之后的不可达字节码不生成。踩坑：`inc_loc`/`dec_loc` 也是写操作，漏统计会让循环计数器被当成常量 → JIT 死循环（已修）。结果：cf(30) 1200→400 ns/call（3×），fibi 保持 9×，递归 fib 1.28×。commit 5182fd6。见 current.md

<14>(SOLVED): Step 11 —— JIT/AOT 后端接口：`JSWhaleJitBackend { name, compile(ctx, src, &entry, &handle), release(handle) }`，当前唯一实现是 TCC；`whale_jit_backend` 指针就是 AOT 插入点（离线 gcc/clang + 磁盘缓存只需实现该结构体），不对"外部编译器"形成硬依赖。行为与性能不变（fibi 仍 9×）。commit 75cd7b6。见 current.md

<15>(SOLVED): Step 12 —— 自查"JIT 没生效"：三个叠加原因（① opcode 覆盖太窄：用户用 `<=` 即 `lte`，而生成器只认 `lt`；② **正确性 bug**：跳转目标的栈深度用了线性扫描的 sp、死代码没回滚 → else 分支写错槽，`h(30)` 给 -29；③ **相对路径**：从 release 目录跑时 TCC 找不到 quickjs.h → 静默回退解释器）。另实现**数值直传**（`whale_jit_core` 全程 double + JSValue 包装层，自递归不再装箱/解箱）。结果：用户的 **fib(40) 17.5s → 2.12s（8.3×）**，递归 fib(24)×20 154→19ms，迭代 fibi 保持 9×。用户实测 1949ms。commit 005c615 + c994c44。见 current.md

<16>(TIPS+SOLVED): Step 13 —— 方向：**直接生成 TCC 的 IR/AST**（用户确认 TCC 可随意改），额外好处是**不再需要 include quickjs.h → `.h` 路径问题从根上消失**，且跳过 C 解析、我们直接持有 IR 可做寄存器保留/窥孔/循环优化；落点 `tccgen.c` 的 vpushi/gen_op/g + SValue/vstack。构建：libtcc 现在**默认从源码编**（源码在 `src/tcc/`），另有 `--libtcc_prebuilt=y` 走预编译 `libtcc.dll`（导入库由 dlltool 生成后入库，构建只 relink 0.8s）。commit ec7287f。见 current.md

<17>(SOLVED): Step 14 —— JIT 生成器拆成独立编译单元 `src/whale_jit.c`（~950 行），引擎用 `JSWhaleFuncInfo` 通信、并提供 `JS_WhaleVarRefIsSelf`/`JS_WhaleCheckStack`/`JS_WhaleOpSize` 三个薄封装。**只改生成器 → 重建 0.5s**（原来 60s）。坑：① TCC 不能和 quickjs 合并 TU（两者都用 `DEF()` 展开 token 枚举，必撞名），要调 TCC 内部 API 就在 TCC 侧加导出封装；② 自己生成 opcode size 表会因为 `quickjs-opcode.h` 重复 include 而错位（`def` 默认落到 `DEF`）→ 改成向引擎要 size。验证：用户 fib.js 2.22s、`bad=0`、`whale api test: ok`。commit 41fe765。见 current.md
<18>(SOLVED 骨架/待办 通路): Step 15 —— `src/tcc/whale_tcc_iface.c` 由 `libtcc.c` 末尾 include，编译在 TinyCC 自己的 TU 里（<17> 结论的正解：不合并 TU，改在 TCC 侧加导出封装）；探针 `JS_WhaleTccProbe()` 返回 `VSTACK_SIZE`=256 证明链路通。附**改动点地图**：x86_64-gen.c 的 g()=146/o()=158/gen_le16=166/gen_modrm=326，tccgen.c 的 gen_op()=2154/vstore()=3069/vswap()=663，tcc.h 的 SValue=424/vstack+vtop=1283/check_vstack()=1305。发 IR 就喂 vstack+gen_op；做窥孔得先在 g()/o() 外加缓冲（TCC 现为单遍流式发码）。见 current.md
<19>(PARTIAL/PLANNED): Step 16 —— IR 直出：`JS_WhaleIrCompileConst2()` 照 `gen_function`(tccgen.c:7054) 的帧序列裸发 IR（不生成 C 源码），**能编过但首次调用崩**（已用 `WHALE_IR_PROBE=1` 开关隔离，正常路径不受影响）。两个发现：单字符运算符 token 就是字符值（`gen_op('+')`）、`TOK_*` 已由 tcc.h:1070 提供别再造。含**崩溃排查方向**、**优先级 2/3 落点与预期**（窥孔 g()/o() +5~15%；寄存器保留 gen_op/gv/vstore +20~50%，且必须 IR 直出通了才动得了）、**扩大受益面**（整数域去守卫预期有限、混合函数分块 JIT 或慢路径内联、外部调用直接调用/通用 helper）、**x86 现代指令集与 SIMD 方向**、**arm 保持兼容**。见 current.md
<20>(PARTIAL): Step 17 —— IR 直出卡在访问违例。**两个架构结论**：(1) TinyCC 的 `tcc_new()` 是"初始化进程级全局环境"而非"可并存实例"（`tcc_state`/`sections`/`table_ident`/`sym_free_first` 全是全局）→ 同进程两次 TCC 生命周期必互踩（实测堆损坏 0xC0000374 → 0xC0000005）→ **通路必须二选一，一次编译=一个版本**；(2) IR 直出**确实不需要 include 路径**（补 `tcc_set_lib_path`/`tcc_add_library_path` 后 `libtcc1-64.a` 错误消失）→ <16> 的"`.h` 路径问题从根上消失"成立。含打点定位方案（①tcc_new ②sym_push2 ③gfunc_prolog ④gen_op/gv ⑤epilog ⑥relocate）与**用户指导：动态类型按参数签名分版本（C++ template 式）**。回归保持绿。见 current.md
<21>(PARTIAL): Step 18 —— 采纳用户诊断：**不建第二个 TCC 实例**（`tcc_new` 会动全局，应改造成"一个实例完成多个任务"；TCC 没有实例下属 ctx）。`JS_WhaleIrCompileConst2` 改为照抄 `tcc_compile()`(libtcc.c:623) 的序列（`tcc_open_bf`→`tccelf_begin_file`→`preprocess_start`→发 IR→`preprocess_end`→`sym_pop`→`tccelf_end_file`→`tcc_close`→`tcc_relocate`），不再用 `tcc_compile_string` 当初始化（它末尾 `preprocess_end` 会拆掉 token 环境 → `tok_alloc` 写已释放内存 = 堆损坏）。打点（`WHALE_IR_DEBUG=1`）结果：`tcc_new`→`paths`→`unit opened`→`tok_alloc`→`symbols`→**`prolog`✓**→`push a`✓→`push b`✓→`gen_op`✓→**崩在 `gv(RC_FLOAT)`**。即建帧/压栈/降级全对，只差"取值到寄存器"。下一步候选：`cur_func_sym` 等"当前函数"全局、double 的 `type.ref` 规范类型、`nosse` 标志、`get_reg` 依赖的发码隐含状态。回归保持绿。见 current.md
<22>(PARTIAL, 只差入口地址): Step 19 —— **IR 直出的机器码已正确生成**。分步打点全线通过（tcc_new→paths→unit opened→canonical types→tok_alloc→symbols→prolog→push→gen_op→gv→gfunc_return→epilog→unit closed→relocated），dump 的字节结构完全正确（push rbp/mov rsp,rbp/sub rsp,0/movq xmm0,[rip+0]/leave/ret）。**三个真 bug 已修**：① 不能拿 `tcc_compile_string` 当初始化（末尾 `preprocess_end` 拆 token 环境→`tok_alloc` 写已释放内存）；② **漏了 `tccgen_compile()`(tccgen.c:235) 开头的全局规范类型**（`int_type`/`size_type`/`ptrdiff_type`/`func_old_type`）—— `gen_op`/`gv` 内部要用，缺了就崩，**"照抄帧序列"不够还要照抄解析器全局初始化**；③ 不要手写 `gv(RC_FLOAT)`（`gfunc_return` 自己会取值）。**唯一卡点**：relocate 后 `text_section->data` 不可读、`tcc_get_symbol` 给的地址（段基址+0x4f8）调用也崩 → 下一步查 Windows/`TCC_OUTPUT_MEMORY` 下正确的入口获取方式（看 `tcc_run`）与常量段是否被搬入可执行映像。回归绿。见 current.md
<23>(PARTIAL, 机器码正确): Step 20 —— IR 直出**已生成完全正确的机器码**（push rbp/mov rsp,rbp/movq [rbp+16],xmm0/movq [rbp+24],xmm1/movq xmm0,[rbp+16]/addsd xmm0,[rbp+24]/leave/ret），参数符号 r=0x132 c=16/24 正确。**关键发现**：① `x86_64-gen.c` 有**两套**代码生成器 —— PE 版 gfunc_prolog/epilog 在 **953/1021**，ELF 版在 1435/1587（`#ifndef PE`）；PE 版参数在**正偏移 [rbp+16] 起**、`loc` 保持 0，参数靠 `sym_push(...)` 新建、要用 **`sym_find(tok)`** 取偏移；② **`sym_push2` 不复制 `CType.ref`**；③ 必须装 `setjmp(s->error_jmp_buf)`（`tcc_error` 是 longjmp）；④ 不能调 `sym_pop`；⑤ **构建坑**：`whale_tcc_iface.c` 改为编译单元并 `#include "libtcc.c"`（原来反向 include 时 xmake 看不到依赖、改它不重编）。**唯一卡点**：机器码/入口/可执行权限都对但调用崩 → 剩疑 PE 的 `.pdata` 展开数据与 `win64_add_function_table`（tccrun.c:305，仅 tcc_run 路径注册）。下一步对照 `whale_tcc_compile`（C 源路径可正常调用）在 relocate 后多做了什么。回归绿。见 current.md
<24>(PARTIAL): Step 21 —— 逐行对照 `gen_function`(tccgen.c:7054) 补上 `tcc_debug_funcstart/funcend`、`gsym(rsym)`、`label_pop`、以及**完整收尾**（`preprocess_end`→`free_inline_functions`→**`free_defines(define_start)`**→`sym_pop(global/local)`→`tccelf_end_file`→`tcc_close`，其中 `free_defines` 漏了会让 `sym_pop` 踩内存）、显式 `VirtualProtect`。**事实**：机器码正确、入口地址正确（`tcc_get_symbol` == `sh_addr`）、权限已加，**调用仍 0xC0000005**。**下一步：做双路径对照** —— 用 C 源路径编译同签名函数（必定成功）并 dump 其字节，与 IR 版逐字节比对，重点看 `.pdata` 展开记录/对齐填充/符号表项与 `pe_add_unwind_data` 的差异；若字节一致则问题在 state 层（临时文件/`runtime_mem`/函数表注册）。回归绿、无性能损失。见 current.md
<25>(PARTIAL): Step 22 —— 双路径对照**未做成**，工具层面踩两坑：① `whale_jit.c` 只 include 公开的 `libtcc.h`，**看不到 `Section`/`TCCState` 内部**（`unknown type name 'Section'`），dump 必须由在 TinyCC TU 里的 iface 提供（已加 `JS_WhaleIrDumpText`）；② 但在 JIT 路径里调该 dump **会把正常 JIT 弄崩**（`qjs` 跑 fib 直接 exit 1），原因未查，已移除并恢复回归。另证伪一条：iface 里新写 `JS_WhaleIrAdd2C()`（另起 state 走 C 源）**比 IR 版更早就崩**，所以对照必须在**已被证明可用的 `whale_tcc_compile` 路径内**做。**下次做法**：改 `whale_tcc_compile` 自身，在 `tcc_compile_string` 后 dump（先只打印 `nb_sections` 与各段 `sh_type/sh_flags/data_offset`、不读 `data`，定位第 2 个坑），再与 IR 版字节逐字节比对。回归绿、无性能损失。见 current.md
<26>(PARTIAL, 可行性已验证): Step 23 —— **外部文件依赖已实测可消掉**。用户指出 C 路径的结构性矛盾是**外部文件依赖**（生成代码引用 `JSValue` → 必须 include `quickjs.h` → 做不成干净的库），方案是 **hook/手写协程覆盖 codegen 的输入**、**优先直接输入 AST**（退路才是 token 流），且"TCC 可随意侵入性修改"。**实测**：依赖的唯一来源是 `whale_jit.c:172` preamble 的 `#include "quickjs.h"`；换成 `typedef unsigned long long JSValue`（64 位 ABI 一致）+ 不透明 `JSContext` + 内联 `JS_EXCEPTION`（`JS_TAG_EXCEPTION=6`，64 位 `JS_MKVAL` 是 `tag<<32`）并**删掉 `tcc_add_include_path`** 后，**JIT 零 include 路径跑通 `fib(32)=2178309`** → <16> 的"`.h` 依赖消失"**从推测变实测**；只差逐个内联剩余常量/原型。本轮一次性加了 `JS_UNDEFINED`/`JS_NULL` 导致 `fibi` 算错，**已回退**（下次一次只加一个符号、跑 fib+fibi 双验证）。下一步：① AST 直出（排查 `block()`/`stmt()` 建的 `define_stack`/`label`/`local_scope` 等上下文）② 退路 token 流（hook `tccpp` 的 `next()`）③ 顺手补完 preamble 去依赖。回归绿、无性能损失。见 current.md
<27>**(SOLVED, 突破)**: Step 24 —— **断开 lexer、接自定义 token 流 → IR 直出通了**。按用户方案：hook `tccpp.c:2990` 的 `next_nomacro()`（`if (whale_tok_active) { whale_next_token(); return; }`），token 源在 iface（同一 TU）：`whale_tok_buf[]` + `whale_next_token()` 设 `tok`/`tokc`/`tok_flags`，标识符必须走 `tok_alloc`，关键字用 `tcctok.h` 枚举；驱动 = `tcc_open_bf`→`tccelf_begin_file`→`preprocess_start`→构造 token→`tccgen_compile(s)`→收尾→`tcc_relocate`→`tcc_get_symbol`。**实测：`[tok] token-stream compile: compiled` / `result = 6.50 (expected 6.50)` / `whale api test: ok`** —— 即 token 流编译出的 `double whale_ir_add(double a,double b){return a+b;}` **可调用且结果正确**。**为什么对**：① 上下文问题彻底消失（`decl`/`block`/`gen_function`/`gen_op` 全是 TCC 自己跑，状态由构造保证正确，不会再出现"机器码正确但调用崩"）；② **外部文件依赖彻底消失**（token 里只有内建类型，无任何 `#include`）→ 印证 <26>。下一步：把 `whale_jit_gen` 从"拼 C 源码"改成"发 token"（类型落到 double/int64，外部函数用 `tcc_add_symbol` + 自声明原型）。见 current.md
<28>(PARTIAL, 根因已找到): Step 25 —— token 通路的根因：**标识符 `out` 撞上了 TinyCC 的 asm 关键字 `TOK_OUT`**（`tcctok.h` 里 asm 助记符 `out`/`in`/`push`/`pop` 也在关键字表内）→ `double *out` 的参数名被吞成关键字 → TCC 报 `function pointer expected`。**定位手段**：dump 生成源码后 `gcc -fsyntax-only` 验证它**完全合法**（一句话把方向从"TCC 嫌语法"扭到"我的词法出错"），再全量 dump token 看到 `[702]out`（其余参数名都是 1269+ 的新 id）。**修法**：给 `whale_keywords` 加白名单（只收真正的 C 关键字）或让生成器避开撞名。本轮 hook/token 源/preamble 自包含/走 token 通路等改动**已全部回退**（文件被改坏且预算耗尽）；其中"无 include 路径下最小 token 通路可跑（result=6.50）"已在 f5e9667 验证。**下一步**：① 修撞名 ② 重做 <26>/<27> 的改动，先只改 `out` 并验证 ③ 把 `gcc -fsyntax-only` 固化为常规自检。工作区回到 f5e9667：whale api test: ok、fib(32)=2178309 44ms（8×）。见 current.md
<29>(PARTIAL, 方向修正): Step 26 —— 用户点破"**你能生成 C 代码不就能自己生成 token 流吗**"，确认我一直在自找麻烦：走的是"生成 C 文本 → 复刻 C 词法器扫回 token"，**麻烦全部来自复刻 TCC 的词法**（关键字表/字面量/运算符），<28> 的 `out` 撞 asm 关键字正是这条路带来的 bug。**正确做法**：`whale_emit` 的 55 个调用点**直接发 token**（`whale_emit("double v%d = 0;\n", i)` → `wtok(TOK_DOUBLE); wtok_id("v"); wtok_int(i); wtok('=')...`），于是不需要词法器、关键字映射缩到几十个已知名字、不会再有 `out` 这类意外；`whale_tok_buf`/`whale_tok_push`/`whale_tok_active`/`next_nomacro` hook（已在 f5e9667）可原样复用。本轮按旧思路再试仍失败（`TOK_SIGNED` 等实际叫 `TOK_SIGNED2`；PowerShell 大段文本替换反复插错位置）—— 这两点本身就是"太绕"的证据。工作区回到 f5e9667：whale api test: ok、fib(32)=2178309 44ms（8×）。见 current.md
<30>(PARTIAL, 两端已接上): Step 27 —— 直接发 token 的**两个真根因**：① `whale_tcc_compile` **一直还在调 `tcc_compile_string`**（生成器已发 token、编译器却读残缺 C 文本，两头没接上 —— 这就是"始终不通"的答案）；② **发 token 必须在 TU 打开之后**（`tok_alloc` 需要 token 表，原先在 `tcc_new` 前跑 → 访问违例），现改为 `JS_WhaleCompileTokens(s, emit_cb, ctx)` 在 `preprocess_start` 后回调。**卡点**：dump 重建的 token 文本只有 952 字节、停在 `{ double v0 = `，而生成器未报 `gen gave up` → **发 token 中途断**，TCC 报 `declaration expected`；下一步从"为何只发到 `v0 = `"入手。**定位手法固化**：`JS_WhaleTokDump()` 用 `get_tok_str()` 把 token 流重建为文本对照（注意 `TOK_INT=256=TOK_IDENT`，`int` 渲染为空是 dump 表现而非缺失）。已完成：全 emit 点改直发 token、`JS_WK_*`/`JS_OP_*` 自有 id（永不写 `TOK_*`，根除 `out` 类撞名）、`whale_load_analyze`+`whale_load_emit`。**代码回退到 f5e9667**（绿），工作保留在 `5716d97`→`0bb5b25`。见 current.md
<31>(TIPS, 重要教训): Step 28 —— 采纳用户反馈"**太依赖测试，跳出来看**"，本轮**只读 emit 代码**就找出 3 个真 bug：① **`wt`/`wk` 混用 37 处**（`wt` 是标点/字符发射器、`wk` 才是关键字；把 extern/int/if/goto/return 发成了控制字符 token，`JS_WK_EXTERN`=8→`\b`），这也解释了 dump 里 `extern`/`int` "渲染为空"；② **`void` 走了 `wt_id`**（应为 `wk(JS_WK_VOID)`）；③ **`JS_WhaleVarRefIsSelf` 原型返回类型写成 `JSContext *`**（应为 `int`）。修完错误由 `declaration expected` → `function pointer expected`（大幅接近，但`decl()` 仍遇到"非类型名标识符 + `(`"，即某个参数列表开头不是类型）。**教训**：emit 点是自己写的，靠**逐行对照 C 语义**即可验证，不必每次构建+运行；对照清单=关键字走 `wk`/标识符走 `wt_id`/类型名与关键字不混/每个 extern 原型返回类型；`JS_WhaleTokDump` 是辅助不是判据（`TOK_INT=256=TOK_IDENT`）。回归于本轮末尾坏、**代码已回退 f5e9667**（绿），工作保留在 `1bdeaae`。见 current.md
<32>(TIPS, 自我纠正): Step 29 —— 上一条 <31> 的结论**是错的**：不是"token 流只发到 `v0 = `"，而是 **`JS_WhaleTokDump()` 自己会崩**（`get_tok_str` 对 `TOK_CINT`/`TOK_CDOUBLE` 会按标识符越界索引 token 表，dump 恰停在 `v0 = ` 后的第一个数字字面量）→ **观测工具故障 ≠ 被观察对象故障**。**纠正方法（可复用）**：把 emit 代码**逐行手译回等价 C**，用 `gcc -fsyntax-only` 检查 —— 唯一报错是我手写时编的标签，即**我读出的序列语法完全正确**。gcc 接受而 TCC 报 `function pointer expected` → 差别不在 token 合法性。**方向已转为**"TCC 没把我们的类型名认出来"：查 `decl()` 里"标识符是否为类型名"的判断路径（`sym_find` 一类）与 `tok_alloc` 出的 token 是否可见（纯读代码，不用跑）。**教训**：① 先读代码（rules 里本就有，我反复忘）② 别信自写观测工具，先确认工具本身对 ③ 观察与推理冲突时先怀疑观察。见 current.md
<33>(PARTIAL+PLAN): Step 30 —— token 通路修复进展（`tcc_open_bf` 缓冲、`whale_tok_active` 守卫提前到 `tcc_new` 前、`JS_WhaleTokCompileAdd2` 补 setjmp、去尖括号文件名；token 流文本+数值双证正确）；仍卡在 `parse_define` 的 `invalid macro name`（最后一个 define 是 `__BASE_FILE__`，值为带引号文件名）。**含用户要求的通用优化方向清单**：**AST 类** A1 循环展开 / A2 循环不变量外提 / A3 归纳变量强度削弱 / A4 小函数内联 / A5 标量替换+逃逸分析 / A6 length 缓存 / A7 常量折叠传播 DCE；**后处理类** B1 窥孔 / B2 跨语句寄存器保留 / B3 SIMD 向量化 / B4 现代指令选择（cmov/lea/AVX2）/ B5 栈槽复用 / B6 分支布局对齐；**运行期架构类** C1 整数域去守卫 / C2 混合函数分块 JIT / C3 外部调用直通 / C4 值类型分层 / C5 任意点 deopt。**排序建议**：B1 → A1 → C1 → B2 → C2 → B3。见 current.md

<34>(SOLVED, 大突破): Step 31 —— **token 直出全程跑通**：生成器不再拼 C 文本，而是直接发 token（`wk`/`wt`/`wt_id`/`wt_slot`/`wt_op`/`wt_int`/`wt_num`），关键字/运算符用自有 `JS_WK_*`/`JS_OP_*` id 映射，**不写任何 `TOK_*`**（根除 `out` 之类撞名）。踩坑与修复：`JSValue` 在非 NAN-boxing 下是 16 字节的 `{union, int64_t}`（曾误写成 8 字节 `unsigned long long` → 每次 JSValue 参数/返回都是垃圾）；`JS_EXCEPTION` 是宏、PE 的 `pe_find_import` 拒绝 `STT_OBJECT` 无 `ST_PE_IMPORT` 的引用 → 生成代码里彻底不用它，失败路径改设 `*deopt`。

<35>(SOLVED): Step 32 —— **单例 TCCState + 多次链接**。TCC 的 `tcc_new()` 重建进程级全局、`tcc_delete()` 会释放各 state 共享的 `sym_pools`，故改为**一个 state 用到进程结束**、每次编译函数名带计数器。让"多次链接"可行的四处修复（全部在 PE 路径）：① `relocate_syms` 用 `st_value += sh_addr` **累加**，且**只应对上次已存在的符号**回落（新增 `nb_relocated_syms` 水位，否则新符号被减成负数）；② `relocate()` 全程 `add32le/add64le` 写进 `s->data`，故加 `relocate_reset()` 把重定位目标清 0（x86_64 用 RELA，addend 在 reloc 表里，初值恒 0）；③ `pe_output_file` **每次**链接都要跑（`pe_build_imports` 要按本次地址绑定 thunk）；④ `pe_output_file` 会设 `runtime_main`，导致下次 `pe_add_runtime` 注入悬空 `main`/`__runmain` → 编译收尾清掉。**最隐蔽的一个**：`pe_check_symbols` 只把 `SHN_UNDEF` 的符号当 import，而链接会把它们 rebind 到 thunk（`st_shndx` 不再是 UNDEF）→ **第二次链接一个 import 都找不到、thunk 不重建、IAT 错位**；修法是记录原符号（symtab 索引）到 `TCCState`，每次 `pe_output_file` 开头 `pe_restore_imports()` 把它们恢复成 `SHN_UNDEF`。诊断手段：gdb 看崩溃 RIP 落在 thunk 填充字节中间 + `WHALE_JIT_DUMP` 打印 `[pe] build_imports: ndlls=… sym_cnt=…`。回归：`f1`/`f2` 双函数 `±4999950000` 全对、不再崩。

<36>(SOLVED): Step 33 —— **回边热度 + 表达式融合 + 代数化简**。① **回边也算热度**：JIT 原本只按函数调用次数计热，导致"调用一次、循环千万次"的函数永不编译；在 `OP_goto`/`OP_goto8`/`OP_goto16` 的向后跳转处计数（编译过一次后被 `whale_jit_tried` 短路，开销可忽略）。② **表达式融合**：栈槽不再立即写成变量，而是持有 token 区间（append-only 工作区 `gw_*` + 定稿流 `go_*`，语句的 token 连续所以定稿是一次 `[mark, now)` 拷贝），物化边界 = 基本块/`put_loc`/`call`/`return`/`if`，并有 24 token 上限防膨胀。`s = s*c+k` 从 4 条语句变 1 条。③ **`fold_inline` 必须加括号**（我引入的 bug）：融合后 `if (<v0>) == 0` 变成 `if ((L0<L1) ? 1 : 0 == 0)`，C 里 `==` 优先级高于 `?:` → **恒真 → 所有循环零次**（症状是所有 MISMATCH 的 check 恰好等于循环初值）。④ **`call` 结果的槽要标记已物化**，否则内联到的是上一次的陈旧表达式（`fib` 曾返回 `(A0)+(A0-1)`）。⑤ **代数化简**（只做消除整条语句那类）：`x*1`/`1*x`/`x/1`/`x-0` → `x`；**刻意不做** `x+0`/`0+x`（`-0+0=+0`）和 `x*0`（`NaN*0=NaN`）。⑥ **`JS_WhaleToInt32` 走截断快路径**：原来用 `fmod`，位运算每轮调 2 次 libm → `bitand` 比 `plain` 慢 9.5×；改为 int32 范围内直接 `(int32_t)d`、`fmod` 只留给冷路径，**378ms → 54ms**。**性能**：`locals` 14×、`fib` 8×、`nested` 从 288→108ms、`arith` 229→109ms、`minmax` 199→159ms，全部 14 个 perftest case 语义与 node 逐一一致。

<37>(TIPS+PLAN): **perftest 设施** —— `perftest/` 下 15 个场景（递归/算术/局部/混合/深表达式/整数循环/分支/嵌套归约/浮点字面量/位运算/代数恒等/数组/字符串/筛法/对象），每个自包含、输出统一行 `WHALE_BENCH <name> ms=<n> check=<v>`；`perftest/main.js` 用 **PATH 命令名**（`node`/`qjs`/`whalejs`）跑，探测不到就跳过，并做跨引擎 `check` 一致性校验。xmake 目标已从 `qjs` 改名 **`whalejs`**，产物名正好就是 perftest 要的命令名。**它抓到的三个静默 bug**（都是"算错但不报错"）：浮点字面量被截断成 int（`OP_push_const` 的 float64 分支没覆盖默认 `WLD_INT`）、`fmod` 未声明导致 TCC 按 int 取返回值 → `%` 恒 0、`OP_not` 被当成逻辑非（QuickJS 里 `!x` 是 `lnot`、`~x` 才是 `not`）。**未完成、待续**：`get_var`（全局数值变量，让 `sieve` 类能进 JIT）、循环不变量外提（LICM）。

<38>(PLAN, 后端方向): **后端指令集扩展方案**已单独记录 —— init 时 CPUID 检测、SSE4.2/AVX2 各做一份替换方案、按支持情况插拔、baseline 只到 SSE2、SIMD 同理。**AOT 路径用户已确认搁置**（当前优化收益已超预期，无实现必要）。见 notes/backend-isa-extensions.md

<39>(TIPS, 待办/用户提醒): **结果缓存（memoization）的容量策略** —— 用户提醒：缓存不能无限增长，而每个程序的规模又不一样，所以将来做结果缓存时要有一个**按轮次定期清理不常用条目**的机制（而不是只设一个固定上限）。**用户明确说"先不急"，所以只是记录，实现结果缓存时必须一起做掉。**

<40>(TIPS, 回滚基准的坑): **`Remove-Item build\.objs` 不足以强制重编** —— xmake 会从自己的缓存恢复对象，"构建成功"却给回**上一个版本的二进制**（实测：旧 tag 只花 2.5s 构建，跑出的数字与新版本几乎相同）。两个正确做法：① `xmake clean -a` 才真正强制全量重编；② 但删掉 `build` 目录会**同时丢掉平台配置**，之后 `xmake -y` 报 `toolchain not found`，必须显式 `xmake f -y -p mingw -m release`。另外还有一个更隐蔽的坑：`jit-opt-0` 那个 revisions 的 target 还叫 **`qjs`** 而不是 `whalejs`，所以脚本必须在构建后把**实际产物**（`whalejs.exe` 或 `qjs.exe`）发布成 `whalejs` 到独立 staging 目录，并每次清空该目录 —— 否则测到的是 PATH 里残留的旧二进制。

<41>(SOLVED + 分析): **延迟物化的语义 bug 已修，且实测出 sieve 的机会在哪**。① bug 成因：`fold_materialize(k)` 把栈槽物化成**固定名字 `v<k>`** 且会被反复重写，而 `fold_inline(k)` 对已物化槽发 `v<k>` —— pending 表达式把这个**记号**记进去了，后续语句重写 `v0` 就静默改了它读到的值（`bitops` 的 `var v = i & 255; acc = acc ^ (v<<3);` 就是 `v` 的 pending 读到了 `acc`）。修法：**每次物化取新名字**（`whale_vserial`），引用走 `whale_vname[k]`，prologue 多声明 `WHALE_VSER_MAX`(64) 个，`OP_get_var` 的 `&v<k>` 与 call 结果槽同样分新名。**`v<n>` 不是 SSA 变量**。② 顺带修的：跳转前 flush（回边跳过 store 导致循环不终止）、`local_inline` 总是内联（否则 `a+b` 变 `a; b`）、`get_loc0_loc1`/`inc_loc`/三处 peephole 目标走延迟物化 helper。③ **sieve 的量化结论**：`whale jit: gen gave up at pos=9 op=33 (call_constructor)` —— 整函数被拒，三个循环全跑解释器。微基准隔离：**300k 次数组写 node 30ms / whalejs 628ms（21×）**、150k 写 18/312ms（17×）、**而 300k 次纯数值循环只差 1.7×（12/20ms）** → 慢的不是循环而是**数组访问**。**下一个最高杠杆 = 让 JIT 支持 TypedArray**（sieve 2111ms 估算可降到 ~150ms，且 array 171ms / object 369ms 同受益）。方案：双域值（`double v<k>` + `void *o<k>`）、`call_constructor` 当不透明值、元素 get/set 走 helper、**必须解决 `o<k>` 的 GC root**（JIT 里的裸指针对 GC 不可见）。见 current.md

<42>(SOLVED + TIPS, 重要教训): **TypedArray 进 JIT + 三处自我纠错**。① **TypedArray 落地**：类型字节携带对象槽号（高 4 位，`WHALE_OBJ_T/IS_OBJ/SLOT`），`call_constructor`/`get_array_el`/`put_array_el` 等 case 齐备，`JS_CallInternal` 在操作数栈与 `var_refs` 之间留 `WHALE_OBJ_SLOTS`(8) 个 GC 可见槽给 JIT 停放对象（裸指针因此安全：对象槽是 GC root，且 accessor 不回调 JS、不会 detach buffer）。`sieve` **2190ms → 178ms（10.4x）**，全量 0 mismatch。② **数组访问必须内联**：走 helper 每元素解析一次 buffer → 第一版可用时**慢 18 倍**（38903ms）；照解释器直接索引 `p->u.array.u.uint8_ptr[i]` 后恢复。再进一步把 **base 指针 + 长度缓存到循环外**（`_op<k>`/`_on<k>`/`_oi<k>`，对象槽被重新赋值时失效缓存）→ 211→178ms。③ **用户发现的稳定退化**：`fib` 9→16ms，根因是对象槽的**每次调用**开销（8 次初始化 + 8 次 `JS_FreeValue` + 返回值各 8 次，还外加 64 字节 `alloca`），递归函数按层数放大。修法：生成器报 `whale_gen_objs`，引擎按函数记 `whale_needs_obj`，**不用就不付**。④ **`WHALE_T_I32`（“已知在 int32 范围内”）**：位运算结果天然满足，作为操作数时 `ToInt32` 退化成一次 cast。**但跳转目标的类型合并把任何差异当作冲突 → 整个函数拒绝 JIT**（`acc` 在一个分支是 `NUM`、另一个是 `I32`）→ `bitops` 226→600ms，且是**静默**的。修法是让 `I32` 和 `NUM` **向下合并成 `NUM`**（`I32` 是优化提示而非值语义）→ 183ms。⑤ **教训（本轮最大浪费）**：`bitops` 掉回解释器时我用 `WHALE_IR_DEBUG` 看 dump 得到空文件，误判成“编译成功但输出丢失”，去找一个不存在的输出问题；真相是某个 `goto out`，而 **`WHALE_IR_DEBUG` 只在编译成功路径 dump，查“为什么没 JIT”要用 `WHALE_JIT_DUMP`**（它一行就打出 `gen gave up at pos=.. op=.. (..)`）。更糟的是我以为诊断不存在，还去加了 37 处 `goto out` 的 `__LINE__` 标记 —— 但 `out:` 处**早就有** `gen gave up` 诊断（`whale_jit.c:403` 的 `whale_op_name`）。**动手前先 grep 现有设施。** ⑥ **规划结论**：语句数最大来源是**表达式超过 `WHALE_EXPR_MAX`(24) 触发 `fold_materialize`**（`bitops` 循环体 24 条语句里 10 条出自此），所以**“缩短表达式”比“删语句”更根本**。下一步方向：**寄存器改造（用户选定，照 8cc/9cc/chibicc 做初版，改 `src/tcc/x86_64-gen.c`）**。见 notes/jit-debugging.md

<43>(TIPS, 速查): **JIT 调试与用法速查**已单独记录 —— 全部环境变量的作用域与语义（尤其 **`WHALE_JIT_DUMP` 查“为什么没 JIT”、`WHALE_IR_DEBUG` 只在成功时 dump token** 这对最容易搞混的开关）、“JIT 没生效”的三步诊断、性能测量的注意事项（`--repeat=N` 取最小、跑基准前要确认没有构建在跑，否则 node 那侧数字虚高）、以及生成器的三层缓冲（token sink → `whale_vexpr[k]` → 输出流）与加 opcode 的检查清单。见 notes/jit-debugging.md

<44>(PLAN, 寄存器改造): 方案已单独记录 → notes/reg-alloc-plan.md。要点：**复用 `register` 关键字**（`TOK_REGISTER` 存在且 `whale_tcc_iface.c:610` 白名单已收，但 `tccgen.c:4121` 目前完全忽略它）—— 这是现成的接口。**8 处改动跨 `tccgen.c`/`tcc.h`/`whale_jit.c`，必须一次做完**（只做“分配保留寄存器”而不做 `gv`/`vstore` 特判，会生成错误代码而非只是没收益）。关键约束：**函数调用会破坏寄存器**，所以初版只对函数体内无 `call` 的函数启用（`whale_jit.c` 侧可判定，且它已有完整的循环体缓冲）。验证要**先用 C 源路径隔离**（手写 `register double` 的小 C，不依赖 `whale_jit.c`）。另记：`SValue.r` 是 `unsigned short` 且 `0x0100`–`0x8000` 全被占满，**别指望在 `r` 里腾新标志位**。**工具坑**：`Get-ChildItem -Include` 不带 `-Recurse` 是**假阴性**（我据此误判成“TCC 没有 `TOK_REGISTER`”并写进了结论，后已复核更正）。

<45>(TODO, 用户点名要做 / 进度): **后端 ISA 分级（SSE2 baseline → SSE4.2 → AVX2）** —— 方案见 notes/backend-isa-extensions.md，**不要漏做**。**已完成的是骨架**（commit bed4f0d、7af225e）：`TCCState.isa_bits` + `WHALE_ISA_SSE2/SSE42/AVX2`（AVX2 蕴含 AVX、SSE4.2 蕴含 SSE4.1，故不留中间层）、`whale_isa_detect()`（CPUID + XGETBV 闸门，`tcc_new` 里调用）、`WHALE_ISA` env 覆盖、`gen_sse_prefix()`/`gen_sse_modrm()` 统一标量 SSE 发射。**待做的是第 3–5 步实际指令替换**，而它们目前**无收益**，原因是我们的 JIT 产物只有 `+ - * /`、比较和转换，**没有 `roundsd` 一类可替换目标**；AVX2 三操作数在 `gen_opf` 里因 `dst == src1` 而等价。**真正能吃下分级红利的是 SIMD 向量化**，而那需要"数组上独立迭代的循环"进 JIT —— 现在只有 `Uint8Array` 那条窄路径。所以顺序上它排在寄存器改造之后，和 `<38>`/`<44>` 是**两条独立的线**（后端 vs 生成器）。





<46>(SOLVED + 评估): **对象返回崩溃根因 = 帧尺寸顺序倒置；部分编译评估** —— ① JIT 函数返回对象崩溃的根因**不在返回路径**：`JS_CallInternal` 用 `b->whale_needs_obj` 算 `whale_obj_n`/`alloca_size`/`obj_slots` 落点，而这个值在 `whale_on_call()` **之前**读 —— 可 `whale_on_call` 正是那个会编译本函数的调用，编译才设 `whale_needs_obj`。所以**触发编译的那一次调用**用小帧跑了新代码，8 个对象槽（128B）写到 alloca 之外、落进 `JS_CallInternal` 的局部变量（`pc`/`sp`/`sf`）。看起来「与代码形状相关」只是因为 `{a:v}`(stack=2) 与 `{a:1*2}`(stack=3) 让越界落在不同局部变量上；看起来「偶发」是因为只有**第一次 JIT 执行**那一趟帧是小的。修法：`whale_on_call` 返回「刚编译」，那一次调用退回解释器，下一次（帧尺寸正确）才第一次跑原生代码 —— 顺带把 JIT 帧内的 JS 操作数栈清成 `JS_UNDEFINED`（`cur_sp` 必须越过 `obj_slots`，GC 因此会扫到它，而 JIT 从不初始化操作数栈）。`array` 190→约 100ms，24 case 0 mismatch。**教训：偶发的内存破坏先查帧布局，别盯出错的那条路径。** ② （评估）用户提的「半虚拟机半 JIT」= 把可编译区间**抽成独立函数**、由解释器传参调用，**绕开了「无法从任意 pc 恢复解释」**（上一轮我据此判定不可行），可行且更干净；JSValue 布局一致 + helper 已是「直操作虚拟机状态」；门槛 = 区间边界（基本块）/ live 槽传参 / 边界处类型假设失效，**最小可行版 = 尾片段**（「循环末大量运算 + 一次 DOM 调用」）。③ object 的「静态偏移 + 动态查表混合体」**现在的 `_ws.fs[]` + `_ws.fp[]` 就是它**（首次查表、之后比 shape 直读指针、`shape_stable` 时零检查），下一步是**多态 IC（2–4 个 shape）**。④ 框架无静态声明 → **shape 就是运行时类型**，已有 `whale_shape_stable` 等编译期分析；缺的是把 IC 提到函数级/全局。见 notes/partial-compilation.md

<47>(PLAN, 路线图): **已知策略的合并评估与排序** 已单独记录 → notes/optimization-roadmap.md。合并 index.md 的 A/B/C 三类与 reg-alloc-plan.md 的框架十项，按「实测缺口 × 地基现成度 × 风险」重排。**第一梯队**：① **尾片段编译（C2/C3）** —— 补 23–35× 的最大缺口（`objfields` 621 / `immutable_update` 693 / `vnode_build` 420 全是「大量数值运算 + 一次外部调用」形状，React/Vue 的 `render` 也是），按用户方案抽独立函数，尾片段因 live 变量几乎全是数值、可拆箱传参、零 GC 风险而最适合起步；② **小函数内联（A4/D1）** —— 与 ① 互补（① 让有 call 的函数能进 JIT，内联让跨函数假设成立）；③ **寄存器保留（B2）** —— `bitops` 8.7× / `ident` 7.2× 是**已进 JIT 的纯数值循环**，慢在每次读写都访存，用户已选定方向且五次尝试的第五次已通。**第二梯队**：④ 物化决策看见引用次数（B7，`WHALE_EXPR_MAX=24` 已实测最优，真问题是单遍看不见后续引用数，`loop_begin`/`loop_end` 缓冲已备）；⑤ 多态 IC（D6，现为单态且 per-call 清零，应一并提到函数级）；⑥ LICM（A2/D5，地基已在，最容易摘）；⑦ 参数特化（D3，两次失败，做前必须先查清第二次为何仍退化）。**第三梯队**：SIMD（B3）→ 再做 ISA 分级（B4，**框架已建但当前无收益**——产物只有 +-*/ 与比较、没有可替换目标，AVX2 三操作数因 `dst == src1` 而等价，红利要等 B3）；字符串拼接（D7）；Array 方法内联（D4）/ Object.keys（D8）；循环交换 + 嵌套扁平化（D10）。**第四梯队**：Proxy 穿透（D2，收益最大风险最高，但 target 的属性访问可直接复用 `JS_WhaleObjFieldPtr`）。**搁置**：AOT（用户已确认）、窥孔/栈槽复用/分支对齐（收益小）、循环展开/强度削弱/标量替换（会被「每次读写都访存」吃掉）。**两条实测反超值得记住**：`object` 28→4ms（0.14×）、`fib` 42→11ms（0.26×）——只要函数能进去且假设成立，现有机制就能赢，**问题始终是「进不去」**。见 notes/optimization-roadmap.md

<48>(SOLVED, 大突破): **值域扩展 —— 闭包调用目标 + 任意值 + 双版本编译** 已单独记录 → notes/value-domain.md。起因：三个最大缺口（`objfields` 621 / `immutable_update` 693 / `vnode_build` 420ms）实际是**同一个形状** —— `var o = make()`，对象来自一次调用，形状只能运行时知道。**障碍一**：`make()` 的字节码是 `get_var_ref0` + `call0`，与自递归（`fib`）**完全相同**，生成器一律读作「被调者就是本函数」（那正是 `fib` 免装箱的原因），于是调用捕获辅助函数的函数入口检查失败、整函数退回解释器。**修法 = 两种读法都编译**：生成器从引擎拿 `whale_assume_self`；`whale_on_call` 先编 self 读法、再编 closure 读法（**两种都可能编不过，且这是预期**）；两种都在时入口检查失败设 `*deopt == 3` —— 它是**唯一**在函数体执行前发生的 deopt，所以 `JS_CallInternal` 只凭这个码切版本，**宿主调用绝不会执行两次**；memo 只按 self 读法建。**障碍二**：新增 `WHALE_T_VAL`（放在对象槽里的任意 JSValue，`WHALE_IS_SLOT`/`WHALE_SLOT` 统一取槽）；调用可以有**活结果**（`JS_WhaleCallValue` 多一个 `ret_slot`，既没丢弃又不可用的结果仍拒绝 —— 日后向它要数值会 deopt 而重跑帧会**再执行一次调用**）；`WHALE_T_VAL` 能在 `get_loc`/`put_loc` 传播并作为 `get_field`/`put_field`/`get_field2` 的**对象**（`o.tag = i` 里 `o` 来自调用），且这类对象**始终保留形状检查**（`stable` 只就本地字面量成立）。调用暂存用对象槽顶部（`WHALE_CALL_BASE`，槽数 8→12），字面量被限制在基址下。**结果**：`objfields` 621→**173ms**，`fib` 保持 ~8ms，24 case 0 mismatch。**剩余**：`immutable_update` 需要**参数作为对象**（`setField(o,v)` 里 `o.a`）= 参数类型特化 D3；`vnode_build` 需要非数值字面量（`push_atom_value`/`null`）与字符串拼接 D7。**工具教训（踩了两次）**：xmake 增量判断不可靠 —— 改 `whale_jit.c` 后 `xmake -y` 仅 3 秒「cache compiling」而二进制没更新，**判断构建是否真发生要看二进制里有没有新符号**，必要时 `xmake -r -y`；`quickjs.c` 有**混合行尾**（少数 CRLF 夹在大量 LF 里），跨行锚点的补丁会因此失配且报错位置误导，脚本应先归一化再做替换。见 notes/value-domain.md

<49>(TIPS, 定位手法): **`gen gave up` 报的 pos 常常不是放弃的**原因**** 已单独记录 → notes/bail-locating.md。查 `immutable_update` 循环体时，dump 说 `gave up at pos=44 op=203 (get_loc0)`，注意力全跑到 `get_loc0` 上；真正放弃的是**跳转目标的状态合并**（`if (differ) goto out;`），发生在处理任意指令之前、恢复快照的那一小段里 —— `get_loc0` 只是恰好是那一轮的跳转目标。**方法**：61 处 `goto out;` 一次性换成 `{ whale_out_line = __LINE__; goto out; }`，`out:` 的诊断加 `bail=whale_jit.c:%d` —— 一次替换永久可用。**坑**：`goto` 不是表达式，**不能用逗号表达式**（`x = __LINE__, goto out;` 报 `expected expression before 'goto'`），必须是块语句。**立刻换来**：`[merge] pos=44 local=0 was=40 now=72` = `VAL(1)` vs `VAL(3)`，**两个都是 VAL 但槽号不同**；而槽统一逻辑是对的（`[put_loc] pos=19 loc_slot=1`），**循环体那次赋值根本没走 `put_loc`** —— 即 `cur[0]` 是被**别处**设成 `VAL(3)` 的（3 是 `whale_objs++` 的第四个值）。**问题未解，但形状变了**：从「合并规则不对」变成「某个未找到的地方在写 `cur[0]`」。**教训**：dump 的 `op` 看起来完全正常（`get_loc0` 不该失败）时，原因在别处。见 notes/bail-locating.md

## <50> One `JS_OP_*` value cannot serve both the condition joiner and the bitwise operator

`notes/operator-table.md` has the detail. Short version: `JS_WhaleTokOp`'s table is
one entry shorter than `JS_OP_*`, so `JS_OP_DOT` falls off the end and pushes
nothing - that is where the empty `()` in ident.js's generated code came from, and
it went unnoticed because every `JS_OP_OR`/`JS_OP_AND` use is inside an `if`,
where any non-zero value reads as true.

Aligning the table fixes the C but breaks `bitops` (60479 -> 56575). Changing
`JS_OP_OR`/`AND` to the bitwise spelling instead makes `bitops` correct. Both
cannot be right at once, so the joiner must become its own value before anything
else in this area is touched.

Next: grep what emits the bitwise `|`, `&`, `<<`, `>>` for a value expression and
see which `JS_OP_*` it asks for. That is the missing half.
## <51> Correction: the operator table was never wrong

`<50>` is retracted. `optok[]` has 17 entries and matches `JS_OP_*` exactly;
`TOK_LAND` has been in it since well before `32dcd44`. The "missing entry" came
from a truncated `Select-String` line - the entry was cut off in the listing, not
absent from the array. `bitops` is 60479 on a clean tree, matching node.

What survives from that round is the observation that bitwise operators are spelled
as literals at whale_jit.c:3099 while `<<`/`>>` go through `wt_op` at 3121, so only
one of the two spellings consults the table.

What is still real and unexplained is the empty `()`:

    function f(n) { var s = 1.0; for (var i = 0; i < n; i++) { s = s / 1.0; s = s - 0.0; } return s; }
    -> L0 = (((L0) - ()));

`s / 1.0` folds away correctly, so the algebraic-identity path runs; the `- 0.0` arm
emits an empty operand. `notes/operator-table.md` now says this instead.
## <52> `whale_pin_alloc` returns -1 because TinyCC runs out of float registers

Full write-up in `notes/pinned-registers.md`. The short version:

`whale_pin_alloc` scans 18..21 (`NB_REGS - 1` is 21), so four float registers can
ever be pinned. With the generator's `nloops <= 1` limit lifted:

    2 pins (lower bound 20):  correct, minmax 118ms
    3 pins (lower bound 19):  hang
    4 pins (lower bound 18):  hang

A pinned register is invisible to `get_reg()`, which has no way to reclaim one, and
when it finds nothing it returns -1 - which its callers treat as impossible. The
lower bound of 18 was already a workaround for this, as its own comment admits.

And even at the safe setting, pinning is slower: `bitops` 165ms -> 337ms. Pinning
permanently takes registers away from TinyCC's expression evaluation and the frame
slot access it saves is cheap. Same conclusion as the earlier `_ws` experiment.

So the first real backend task is specific: **let `get_reg()` spill a pinned
register under pressure** - write it back to its frame slot and update the map so
later reads go to memory again. Until that exists, pinning is a claim the allocator
cannot veto, and it is either unsafe or unprofitable. That is the precondition for
any no-stack calling convention to be worth doing.
## <53> Debugging discipline: 0 mismatches is not proof the JIT worked

Full write-up in `notes/debugging-discipline.md`. Two rules came out of the
`sym->r` revert, both worth more than the change was.

**1. Check generation errors, not just checksums.** `070a5ee` reported "all 24 cases
match node, timings unchanged". Both were true - and both were also true of a build
where every JIT compilation failed and every benchmark fell back to the interpreter.
0 mismatches means the answers were right, which the interpreter guarantees. The
compiler's own errors are swallowed by the fallback. So every benchmark run needs:

    whalejs case.js 2>&1 | Select-String "lvalue|invalid macro|: error|expected"

`gen-errors=0/24` goes next to the mismatch count from now on.

**2. Six patches on the same mechanism means the model is wrong, not the code.**
The pinned-register work accumulated six special cases in tccgen.c (953, 1056, 1408,
2276, 3331, 4277), one of them saying "Without this the pin did nothing but waste a
register" - a description of the last hole rather than of the cause. The cause was
that `sym->r` was never changed, so the symbol table thought the value lived in the
frame slot while it lived in a register, and every access needed its own patch. Worse,
the patches are load bearing *against* the fix: they set `vtop->r` to VT_LOCAL, which
contradicts `sym->r` - which is why setting `sym->r` alone broke code generation.

Auditing for this class:

    Select-String -Path src/tcc/*.c -Pattern "WhaleJS:|without this|otherwise|work around|avoid"

That finds the pin cluster and nothing else of comparable shape. The remaining WhaleJS
changes in TCC are additive (a new allocator, a new reset hook), not corrections of
existing behaviour.

Still verified and still worth having: pinning pays 25-30% when the right locals are
chosen, and two pins is the safe maximum (a third hangs on get_reg() exhaustion).
Next: remove the six patches and make `sym->r` authoritative in one change, then
weight inc_loc/dec_loc and lift the nloops restriction.
## <54> Two pinning findings: whale_nloops was always wrong, and reading order changes the result

`notes/pinning-findings.md` has the detail and the repro. Summary:

**1. `whale_nloops` was never right.** `loop_back[]` holds -1 for "no back edge", and
the count tested `if (loop_back[k])` - true for every entry. `arith` has one loop and
reported 21, so `nloops <= 1` was effectively false for every function longer than a
few bytes: **the pin gate was off, which is why no pin ever took effect inside a
loop.** Fix is `>= 0`.

**2. With pins on, the result now depends on read order.** Same generated code - the
token streams are byte-identical - but:

    console.log(check); console.log(check);        -> 2398830 / 2398830  OK
    console.log(ms);    console.log(check);        -> 0 / 406            WRONG
    console.log(check); console.log(ms);           -> 406 / 0            WRONG
    console.log(check * 1); console.log(ms * 1);   -> 2398830 / 142      OK

Read it first with nothing before, and it is right; touch anything else first and it
is stale; multiply by 1 and it refreshes. A copy survives somewhere unrefreshed.

## <55> Mixed compilation: the fallback made 10 more cases compile - then its wiring corrupted memory

Detailed write-up in `notes/whale-step-investigation.md`, and `notes/current.md` <55>.
The general fallback of `notes/mixed-compilation-boundary.md` was implemented end to
end (engine-side `JS_WhaleStepRun` + `whale_step_emit`, wired into `default:` and the
five refusals). `gen gave up` across the 24 perftest cases went from **23 to 10**.

Two real problems came out of it, and only the first is fixed:

**1. The loop buffer was writing out of bounds and losing code silently - fixed.**
`WHALE_LOOP_BUF` is 4096, but `fold_commit()` and `loop_end()` both bound the append
with `WHALE_TOK_MAX` (32768). While a loop body is open `whale_out_tok` points at
`WhaleLoop.tok[4096]`, so a body past 4096 tokens wrote past the end of it, and
`loop_end()` dropped the tail without a word. Nothing had hit it because the buffer
has never been used for a loop-level pass - it is dead weight. `loop_begin` now
returns 0, which is the "stack is full, emit straight through" path it already had.

**2. The wiring widens what compiles, and that walked into latent bugs - closed for now.**
Three separate things came out of this, in the order they were found:

- **My own misreading.** An earlier revision blamed `JS_NewArray` returning
  `JS_TAG_UNINITIALIZED`. There is no such thing here: `JS_TAG_OBJECT` **is** `-1`
  (quickjs.c:188), so `tag = -1` was a perfectly good object all along. That whole
  line of investigation was a wrong turn, and it cost most of a day.
- **A real latent generator bug - fixed and committed (`e01fe97`).** The
  array-element unboxing in `OP_get_array_el` spelled member access `->` where the
  operand is a `JSValue` (a value, not a pointer). TinyCC reported `pointer expected`
  and refused every function that read an array element as a number. The baseline
  never emitted that path - the reproducer does not compile there at all - so it is
  latent, and widening what compiles is what walks into it.
- **A real gap in the diagnostics - fixed and committed (`e01fe97`).** TinyCC's
  errors always said `whale_jit.c:1`, because the unit is fed as tokens and has no
  real lines. `JS_WhaleCompileTokens()`'s `setjmp` path now prints `whale_tok_pos`
  and the surrounding tokens, which is what located the `->`.

**Resolved as a restriction, and the fallback is in the tree (`9b8481a`).** The fault
is specific: a **parked object in a value position**. `out[i] = t` dies with
0xC0000005 where `out[i] = 3` is fine, with nothing different on the engine side - so
`whale_step_emit` now accepts a parked object only as operand 0 (the receiver of a
property access) and requires every other operand to be a number. That is enough to
make the mechanism safe: all five reproducers agree with node, all 24 perftest cases
produce correct output, and `gen gave up` falls from 23 to 14.

**Unboxing: built, measured, on by default (`e5a5eef`).** `OP_object` lays a settled
shape out as a frame struct, and the four field access ops read and write its columns
directly - one access, no shape check, no pointer chase, no tag branch.

    WHALE_NO_UNBOX=1   object   7ms   objshape  823ms
    default            object   4ms   objshape  563ms
    stock qjs          object 386ms   objshape 1241ms

**This entry previously said the opposite, and the mistake is worth keeping.** An
earlier revision had unboxing off by default and recorded "19-50x slower than node",
attributing it to statement count. That was wrong at every level:

- the struct was being *created* while the accesses still went through
  `JS_WhaleObjFieldPtr` - the half `fc44fa4` had withdrawn as useless on its own;
- `WHALE_IS_STRUCT` is deliberately not part of `WHALE_IS_SLOT`, and the struct case
  was placed *inside* the `WHALE_IS_SLOT` branch of `put_loc`, i.e. unreachable. The
  deferred-store path then emitted `L0 = ( )` - a local assigned an empty expression -
  and TinyCC refused the function (`identifier expected`, token 998). `get_loc` and
  `set_loc` needed the same treatment;
- so the timing was **the interpreter**. `temp/obj_ubx/` was empty: no machine code
  was ever produced for that configuration.

The general lesson is the one `notes/debugging-discipline.md` already states in
another shape: **"slow" is not proof the JIT ran**, any more than "0 mismatches" is.
The cheap check is stock qjs on the same case - an interpreted whalejs time sits
almost exactly on top of it, so closeness to qjs means it never compiled.

**State**: tree green at `e5a5eef`; 24/24 perftest correct; `WHALE_NO_UNBOX=1` and
`WHALE_STEP_OFF=<opcode>` are the switches, and `temp/w_*.js` the reproducers.

`check`/`ms` are top-level locals (interpreted, not compiled), so the corruption comes
from `build`/`scan`, which pin both `i` and the return variable `s`. That is the same
shape 7d9247b fixed for the old condition ("returning the pinned one unconditionally
left the caller reading a register the value was never in - `return s` came back as
the loop counter"), reintroduced by pinning the return variable again.

Reverted to 38579bc; both findings are recorded rather than half-fixed.- `<56>` `notes/current.md` - five JIT fixes in one session: return-a-call's-result
  (72ms -> 16ms), call-argument materialisation, one materialisation slot per layout,
  `bail=N` diagnostics + a wider object-slot budget, and self-calls with a parked
  object. Method that found them: stock qjs on the same case, then `bail=N`. Blocker
  left in immutable_update: the jump-target merge treats two different object slots as
  a conflict, because a slot's identity is its slot number.
- Update to `whale-step-repro.md`: taking the value-position fence down shows the failure is at RUN TIME, not in TinyCC (exit 0xC0000005, after the first call returns). Cell mapping, operand layout, tags and the JS_SetPropertyValue call are all ruled out with evidence; the remaining suspect is an ownership mistake in the hand-over that only bites on the second call. Use ASan if a build can be arranged.
- Update 3 to `whale-step-repro.md`: the argument path. Struck out last round's ''order'' objection (the scan pass finishes before emit, so the layout IS known at get_arg). Pre-deriving it for the whole function breaks OP_object/put_loc (objfields 64->168ms); deriving it lazily works (75ms) but crossfn still goes 159->578ms, because the struct cannot cross the call - run materialises, use lays out, 16 engine calls per iteration. Correct but not sufficient; needs inlining or columns crossing the boundary. Reverted.
- Correction to Update 3 (`whale-step-repro.md`): the user was right. 578ms was the INTERPRETER (qjs control 584ms), not a boundary cost - the change made `op=155 (mod)` give up with `ta=26` (WHALE_T_STRUCT) at `sp-2`, dropping the whole function. The argument path is not ''correct but not worth it'', it is broken: the struct changes what sits at sp-2 for the next opcode. Also confirmed: materialising at a compiled-to-compiled boundary is wrong in principle - the laid-out form should cross as columns.
- Update 4 (`whale-step-repro.md`): retrying the argument path fixed a real bug and exposed another. **`cur[idx]` is the argument NUMBER, not its local slot** - writing a struct type there retyped an unrelated local (`i`), which is what made `mod` give up and the 578ms interpreter speed. Retyping only the stack copy removes the give-up, but the answer is then `check=0` (every field reads 0) - a silent wrong answer, and 433ms vs 163ms. Reverted. Remaining: check JS_WhaleObjFieldNum's failure path, and cross the boundary as columns rather than re-laying-out per read.
- Update 5 (`whale-step-repro.md`): (2) checked - `JS_WhaleObjFieldNum`'s failure path is CORRECT (`goto bad` sets `*deopt = 1`), so `check=0` is not a swallowed failure. The shared shape cache theory was ruled out by measurement (`{a:11,b:22,c:33,d:44}` is right on all three, and `put_loc`'s existing layout uses the same shared slot). Also records the user's design: the `JSObject` keeps its identity and carries a pointer to a native C struct that machine code operates on directly, interpreter pays one check - one object, two views, no materialisation at any boundary. That supersedes the ''correct but not sufficient'' framing, and inlining is not the only route.
- Correction (`whale-step-repro.md`): the native form is a new OPTION IN THE EXISTING UNION of `JSObject`, not a new field - object size unchanged. Verified: `struct JSObject` (quickjs.c:1169) already ends in a union that includes `void *opaque`, so it is 8 bytes wide regardless.
- **`notes/whale-struct-object.md` - the C-struct object architecture, stated once and precisely.** A new kind of object (not an extension of JS_CLASS_OBJECT): `{offsetMap, dynamicMap, data}` - the offset map is name->column and holds no values, the dynamic map holds only the names the offset map does not cover (ordinary shape/prop), `data` is the flat JSValue array the generated code addresses by compile-time offset. Lookup order is offset map then dynamic map on both sides, and the two name sets are disjoint so nothing is stored twice. `in` consults the offset map, not the value; an unassigned column is tag=3 with payload!=0, an assigned undefined is tag=3 with payload==0, so there is no `set` flag. Records every WRONG reading made during the work (native form bolted onto JSObject, shape-as-offset-map, `set` flag, duplicated values, materialising at boundaries, frame-side `_ws.sv[]`) and the verification discipline (run node and stock qjs before concluding anything).

- `<57>` `notes/current.md` - native control-flow compilation (for/if/while/switch) preparation: where the change goes, predicted bugs, verification plan, order of work. Key correction: objfields' bottleneck is not the object (same speed as the identical loop body with plain locals); the older 23x note is withdrawn. See notes/native-control-flow-plan.md
- `<58>` `notes/current.md` - compiler-side loop table: compute_stack_size now records back edges (tgt < pos) into a WhaleLoop table on JSFunctionBytecode (inline array, WHALE_LOOP_MAX 64), so the generator is told the loop instead of inferring it. Verified: all perftest loops found (sieve 4 incl. nested), 0 mismatches, no perf change. See notes/native-control-flow-plan.md
- `<59>` `notes/current.md` - try/catch now compiles and is correct (three bugs: core returning a JSValue, the catch slot's fold state, and nip_catch having to *move* a value rather than just drop one). Slower than not compiling (225ms vs its own interpreter's 156ms vs node's 5ms), so no win yet; the per-call costs and the direction are in notes/exception-optimisation.md, which also records the outstanding C-baseline (-O0/-O1) comparison that everything else should be judged against.- `<60>` `notes/current.md` - immutable_update (a Redux reducer) was never compiled: it
  reported 549ms with NO entry at all. Root mechanism found: `whale_ret_obj_g` in the
  scan pass only recognises `return v` (a local), not `return {literal}`, so
  `setField`'s returned object was laid out as frame columns and then left the
  function. Six layers recorded: return refusing a struct; the `_ws.wa[]` field never
  declared; the shallow return test; `OP_object`'s frame-column arm missing the
  "no call after this point" gate that `put_loc` has; object slots not unified between
  `put_loc` and `set_loc` (the user's "the graph is wrong", not a merge-policy
  problem); and a struct vs plain object in the same slot. Reverted: after all six it
  compiled and ran fast but gave `check=0` - a seventh layer puts the literal inside
  the loop body. Baseline is correct-and-slow; correctness does not trade for speed.
  Start again from the call gate + slot unification. Verified after revert:
  0 mismatches over 25 cases, `whale api test: ok`.
<61>(SOLVED + PLAN, 本轮): **补丁清理 + 两个关键 bug + struct 统一字段协议**。① vnode_build `'L3' undeclared` 的根因是未提交 WIP 把 struct 列声明用 `whale_shape_stable` 门住、而 put_loc 填充路径用另一组条件照样发使用 —— 声明与使用必须同源，现在无条件声明（16 个 double 的代价）。② immutable_update 的 flag=1 deopt 退役循环根因是 `JS_WhaleObjFieldPtr/ObjFieldNum` 只认 JS_CLASS_OBJECT：setField 第二次调用收到 struct 对象即 NULL→deopt→retire→解释器。struct 现在与普通对象共用同一 `JSValue*` 字段协议（列地址即 field pointer，WhaleStruct 本身即缓存键）；补 JS_SetPropertyInternal2 写 hook（否则中途 deopt 后解释器写丢列值）。③ 寄存器 pin 全套删除（生成器 `register` 发射 + TCC 八处特判 + Sym.pin_reg）——TCC 下 5-12% 上限、gcc 下灾难（16 个 register 提示打爆分配器）、六处访问路径特判被 notes 定性为对抗正确修法。closure 57→10ms、props_diff 125→22ms 是删除的意外收益。④ 运行时热路径 getenv 全部缓存（`whale_str_stat_on`/`whale_step_dbg_on`/DeoptNote）——每次查列一次 getenv、每迭代九次，是 31 秒首测的最大单项。⑤ offset map 从 JS 对象改平行 atom 数组；dynamic map 惰性；**js_whale_struct_gc_mark 注册**（列里的 JSValue 和 dynamic 此前对 GC 不可见——vnode 的 props 字段会被回收，这是比性能更重的隐患）。⑥ gcc 后端须给全路径（裸 `gcc` → gendef 拼出 `\gendef.exe` → 静默退回解释器，比 TCC 慢 3.6×）。**结果**：25/25 对；immutable_update entries=1000920（百万次调用全进编译代码，但每迭代仍一次真实跨函数调用+struct 三次分配，468-538ms ≈ 解释器速度——下一步是内联/D3）；**objfields+gcc 17ms 反超 node 19ms**——c_native/objfields.c 的"天花板"被 `fmod` 完整 double 语义压低（gcc 无法整数化 libm 调用），JIT 的守卫 idiv 在 JS 语义内合法更快，"比 C 还快"不可疑。**未编译/待做**：string(string 的 call_method)/stringcat(add_loc 的 opaque-local 通路——需要 local 的 VAL 槽写回，同 <60> 第五层的 loc_slot 坑，方案已具体化)/objshape 的 escaped()（put_array_el 带对象值）/array/list_reconcile/nested2d（数组参数 D3 + get_length + put_array_el 值版 + 嵌套 ARR 元素）；props_diff 在 gcc 后端 614ms（默认 flags 疑似无 -O，后端轮处理）。见 current.md <61>

<62>(SOLVED + PLAN, 本轮): **env 重构 + 25/25 全编译 + BinopVal 栈约定修复 + fresh struct 直写**。① **环境变量按类处置**：C 类功能开关改为 `JS_WhaleSetJitCc(driver, flags)` API（库不该用环境变量传策略；gcc 后端必须给完整路径）；A/B 类诊断全部进 `--whale_debug=y` 条件编译（`src/whale_dbg.h` 的 `whale_dbg_env()` 宏，release 二进制无读取无打印）；`WHALE_ENTER_COUNT` 保留（perftest 的 entries 列协议，非诊断打印）；D 类上游不动。② **四条新通路让所有 case 编译**：`OP_call_method/tail_call_method`（`JS_WhaleCallMethodValue`，receiver 作 this）；`add_loc` 的 opaque 通路（`JS_WhaleBinopVal` 写回本地槽，槽身份不变）；`put_array_el` 的对象值（`JS_WhaleArrSetVal`）；数组参数（`whale_feeds_array_use` 前瞻 + `JS_WhaleArrBind` 绑定 + `WHALE_T_ARR` 类型）。③ **两个静默错误答案 bug**：`JS_WhaleBinopVal` 把本地 `v[2]` 当解释器栈传给 `js_*_slow`（它们读 `sp[-2]/sp[-1]`）→ 全部 opaque 运算是无操作，操作数布局改为 v[2]/v[3] + 传 v+4；JIT 入口的 `sf->cur_sp = NULL` 是未撤的 TEMPORARY EXPERIMENT → GC 扫不到 obj slots，长循环分配触发的 GC 会回收槽内对象，恢复为 `obj_slots + WHALE_OBJ_SLOTS`。④ **fresh struct 直写**：`JS_WhaleStructDataPtr` 一次解析，`define_field/put_field` 在 fresh 窗口（无 call/跳转目标）内直接 `_sdp[col].tag/u.float64` 赋值；布局来源改用字面量自身的 `define_field` atoms（工厂函数没有访问 site，`mono` 恒假 → 落入 ObjNew 兜底 → StructSetNum 拒绝普通对象 → 每调用必 deopt 退役），兜底 arm 配 `ObjDefineNum`。**结果**：25/25 编译、0 mismatch；props_diff entries 39808（两个函数都编译）17ms；vnode_build 10ms；immutable_update 全量编译 491ms（每迭代一次真实跨函数调用 + struct 分配是剩余成本，靠内联/D3）。**gcc 后端走 API**：`JS_WhaleSetJitCc("C:/mingw64/bin/gcc.exe", NULL)`。**下一步**：vnode/diff 剩余瓶颈 = 每调用包装（CheckStack/ArgGetObj/_ws memset ~2.7KB/DeoptNote/memo）≈ 执行体的 20 倍，突破口是内联（A4）与 `_ws` 按需定尺寸；objfields-gcc 已反超 node（17 vs 19ms），c_native/objfields.c 基线被 fmod 完整语义压低，建议基线改 `(long)i % 7` 才是同口径天花板。见 current.md <62>

<63>(SOLVED + 测量, 本轮): **objshape 分相定位 + 逃逸对象真 struct + 单块分配**。objshape 528ms 拆相实测（1.2M 迭代/相）：twoShapes 47ms、growsShape 16、writeOrder 20、**escaped 430** —— 全部时间在第四相。escaped 的 `t = {v,u}; out[i&7] = t` 每迭代把对象存进数组：OP_put_array_el 现在和 OP_return 一样把函数标记为"交出对象"→ 字面量直接 StructNew 真结构体（列随对象走，数组存引用，**零物化**）；fresh 窗口内的 get_field 也直读 `_sdp[col]`（tag 内联检查）。`JS_WhaleStructNew` 改单块分配（header+atoms+data 一次 malloc，finalizer 一次 free）——immutable_update 每迭代一次 StructNew，100 万次兑现：**491 → 392ms**。**诚实记录**：escaped 本身没变快（413-680 vs 430）——墙是 `JS_NewObjectClass` + 每次分配触发的 GC 扫描，node 赢这个形状靠**逃逸分析标量化**（t 根本不分配），不是更好的对象。**回答用户三问**：① 优先级 = 是，两把刀不同：escaped 是"物化+分配"，immutable 是**每迭代一次真实引擎调用**（JS_Call 全帧 ~250ns × 1M）+ fver++ 每代 6 次 fp 重解析 helper。② objshape 的 CStruct：twoShapes 的第二形状 b 走 plain+fp 查表路径（单布局限制），但它的成本不是大头；大头是 escaped 的物化，已修。③ dynamic 表缓存 = fp 缓存已是"一属性一解析"语义（解析一次直读，残余是每访问的 shape 校验 helper）；shape_stable 时校验整个跳过（零调用）。**再往下压的三步（下轮）**：a) **compiled-to-compiled fast call**——JS_WhaleCallValue 检查 callee 已编译则绕过 JS_Call 的解释器帧装配（最小帧：current_stack_frame/arg_buf/var_refs/prev_frame），immutable 预期 -200ms；b) var_ref 只 park 一次（现每迭代 VarRefGetObj）；c) 每代 shape 读合并（fver++ 后第一个 site 解析、同代共享）。分配器侧：JS_NewObjectClass 的替代（轻量类对象路径）或真新生代。见 current.md <63>

<64>(SOLVED, 本轮): **compiled-to-compiled fast call + closure 值 prologue park**。`whale_fast_call`：callee 已编译时绕过 JS_Call 的分发/hook/参数拷贝，caller 的 obj slots 直接借用为 callee 的 arg_buf（零拷贝），只建不可省的帧本体；deopt 语义严格保持（丢快帧 + 全新 JS_Call 重跑，re-arm 计数同步维护），deopt==3 换读法在快帧内原位重入。约束 argc==arg_count 且 var_ref_count==0。closure reading 的调用目标槽 prologue park 一次（scan 新增 `var_ref_val[]`，`var_ref_used[]` 语义收窄为 entry self-check）。immutable_update 392→342ms。**教训**：调用开销估算要分"帧装配（不可省）"与"路径杂费（可省）"，实测省 ~50ns/调用而非估算的 150。**下一步不变**：内联（A4，var_ref 解析 callee 字节码 + 身份守卫的 D3 式推测内联——`JS_WhaleVarRefFuncInfo`）是 immutable/框架形状的真解；分配器（JS_NewObjectClass 轻量路径/新生代）是 escaped 的墙。见 current.md <64>

<66>(PARTIAL, 本轮): **var_ref 推测内联建成、实测变慢、单点关闭**。机制全在树内（`41a5ebc`）：`JS_WhaleVarRefFuncInfo` 编译期解析（current_stack_frame 是正在编译的帧，var_refs 活着）、`JS_WhaleVrExpectAdd/VarRefIsFn` 守卫（指针以表索引 travels，token 层无 64 位字面量）、`WHALE_T_VRFN`（字节编 var_ref idx）、`whale_inline_body` 参数类型继承调用点 + 内联体 OP_object 走 StructNew/fresh 直写、`whale_str_*` 提文件级 static。三个坑已修：cur[idx] 一格两用（var_ref idx ≠ local idx，step 的 i 被污染成函数类型）、gen 无 var_refs（走 current_stack_frame）、声明顺序。**实测变慢**（immutable 342→549 且 entries 归零；objfields 同）→ 解析点 `if (0 && ...)` 单点关闭，一切恢复 immutable 327ms。**下一步第一件事**：dump 两个读法的内联流找变慢原因（truncate/re-type 与 general 分支 fresh 物化段的交互是首要嫌疑）。escaped 的 StructNew-per-iteration 实测 612 vs 物化 430，逃逸标记已回退（墙是 JS_NewObjectClass）。见 current.md <65>

<67>(SOLVED, 本轮): **closure 槽 = 存储单元**。`JS_WhaleVarRefPval` prologue 一次取 cell，get_var_ref/put_var_ref 变 tag 检查的直读直写（写侧因 tag 检查已证明旧值 immediate 而免 JS_FreeValue）——`n=n+1` = load+add+store 零 helper，"普通函数+闭包上下文"形态，循环路径零装箱。closure 10-11→9ms（剩余=tag 分支+访存，TCC 无提升；循环内寄存器化需 LICM 暂缓）。坑：组合名必须 `wt_slot`（`wt_id("_p")+wt_int(0)` = 两个 token）；三处 `return 0 }` 漏分号同批引入已修。见 current.md <66 续>

<67>(TIPS+UNRESOLVED, 本轮): **TCC/ELS 调研 + vr-inline 修复清单**。源码量化：tccgen 6986 行、**单遍发码 g()×1487 + o()×3270 ≈ 4800 直发点**、无 IR 无活跃分析（设计选择非 bug）。ELS 路线：A 改 tccgen 本体（gen_modrm 寻址/发码熔合是硬骨头，2-4 周高回归，不建议）；**B 生成器侧 ELS（推荐）**——我们的输入是自产规则子集且全函数体已在缓冲（loop 缓冲+循环表），直接产带 ELS 的线性 IR 绕过 tccgen，估 800-1500 行，编译时长代价微秒级可忽略。**收益上限参照 gcc 后端实测（数值循环 1.5-4x），ELS 只覆盖分配维度，诚实预估 1.3-2x；优先级低于内联、高于 tccgen 改造**。建议排序：① gcc 后端做热点专用（两级 JIT，先测延迟分布）② 路线 B ③ 路线 A 永不。另：vr-inline 的三个正确性修复因实测变慢随分支丢弃，重新启用需重做（修复清单完整记录 current.md <66 续>）；_ws 选择性清零实测有隐藏读者（props_diff=99）已回退全清零。见 current.md <66 续>

<68>(TIPS, 当前结论 + 接下来任务汇总): **结论**：① 25/25 case 全部可 JIT、0 mismatch；性能榜（runner 口径）：immutable_update 327ms / objshape 516 / props_diff 15 / vnode_build 13 / closure 9 / object 3、fib 13（反超 node）。② 反超/接近 node 的：object、objfields(gcc)、fib、stringcat、closure、arr_methods；**剩余缺口两族**：immutable/objshape 的"每迭代对象分配 + 跨函数调用链"（墙 = JS_NewObjectClass 与 JS_Call 全帧装配）与纯数值循环的 TCC 代码质量（gcc 后端已证 1.5-4x，见 <67>）。③ 环境变量已按类重构（C 类走 JS_WhaleSetJitCc API；A/B 类进 --whale_debug 条件编译）；寄存器 pin 全套已删；BinopVal 栈约定与 cur_sp GC 窗口两个静默错误已修；struct 与普通对象共用字段协议 + gc_mark 注册；fast-call 最小帧 + 闭包槽 pval 一次取直读写已落地。④ **vr-inline 机制建成但实测变慢已单点关闭**——重新启用需先重做三个正确性修复（清单见 current.md <66 续>）并诊断第二读法的拒绝链。**接下来任务（按值排序）**：1. **vr-inline 诊断转正**——dump 两读法内联流找 549ms 拒绝链根因，转正后 immutable 的每迭代调用链（~150ns）整体消失；2. **分配器**——JS_NewObjectClass 的 struct 专用轻量路径或新生代 bump（escaped 相 612 vs 430 的墙）；3. **两级 JIT**——TCC 冷启动 + gcc 热点换装（JS_WhaleSetJitCc 已在，加"热点再编译"机制，先测延迟分布）；4. **生成器侧 ELS**（路线 B，见 <67> 调研）——数值循环 1.3-2x 的中间档；5. fast-call 约束放开（argc 补 undef、带 var_ref 的 callee）；6. spread 识别 → 预分配 + push 的原生形状（proxy 已结构性安全：快路径 class 检查 → deopt → 解释器触发 trap）。

<69>(SOLVED + 休眠): **vr-inline 机制修通但测量决定休眠 + getenv 探测 bug**。五个真 bug：① 内联体 StructNew 少发 `ctx`（fn_2 在 TCC 层编译失败——上轮 549ms 的直接原因）；② token 提交交错（体直写 + 调用点重拷整段 → 守卫错位/体重复/get_arg 残片 → 修法是体写独立缓冲、守卫先行体后追加，cpool 路径同改）；③ `whale_vr_expect_slot` 从未写入（守卫永失败，补落库）；④ 体复读参数槽（get_field 把对象槽重写成 NUM → 快照参数类型/表达式/物化态）；⑤ 失败体污染调用点（快照恢复 + 补 vserial）。修通后 immutable 反而慢 40~80ms：helper 总数相等、守卫免费、生成码无冗余，差额 = TCC 无寄存器分配（体内 ~40 语句全走内存，不如快帧 50ns）→ `WHALE_VR_INLINE` 开关休眠（debug 可用、release 硬关），开关开 25/25 全对，转正等生成器侧 ELS。**附带**：`JS_WhaleDeoptNote` 在 `WHALE_ENTER_COUNT` 未设置时每次返回走 getenv（+2.7s，runner 恒设该变量故历史测量全"干净"，裸跑全赔）→ 单次探测，裸跑 2950→310ms。**教训：runner 口径 ≠ 用户口径，两边都要对照。**见 current.md <69>

<70>(SOLVED): **两级 JIT 的 gcc 一半：按函数指定的 gcc -c 后端，TCC 进程内链接**。`pe_load_coff`（tccpe.c 新，~200 行）把 gcc 的 PE-COFF .o 翻译进 TCC 内部表示（ALLOC 段并入、defined 登记、undefined 留给 `pe_build_imports`、REL32→PC32；`.pdata/.xdata`/debug 跳过）。两个真 bug：① PC 语义差 4 字节（COFF 的 P 是字段末尾，TCC 用字段起始——-4 走 RELA addend 而非补数据，因 `relocate_reset` 会洗掉后者）；② `tcc_add_file` 不经过 CompileTokens 尾声的 `runtime_main=NULL` 清理 → 误注入 `__runmain` + CRT（iface 新导出 `JS_WhaleTccClearRuntimeMain`）。按函数指定：`whale_jit_gcc` + `prefer_gcc` 三处透传 + `JS_WhaleSetJitCcForFunction`，默认全 TCC。旧 `.dll`/gendef/dlltool/`LoadLibrary` 整套退役（~100 行删）；as 拒绝 `-o -`，故 `.o` 落 TEMP 临时文件（源码仍走 stdin）。测量：同形状 400×fib(30) TCC 4ms（含编译）vs gcc 252ms（含编译）/**纯执行 1ms**——执行体 3-4x 符合预期，编译 ~250ms 故小函数不划算，"热点再编译"需按执行/编译比设门限。见 current.md <70>

<71>(SOLVED + TIPS): **编译阈值分级 + clang 说明 + 技术债清单**。`whale_hot_threshold(b)`：嵌套循环→10、大单循环→20、小单循环→50、无循环→100（保持）；三处调用点全换，profiling 窗口（8 次绝对值）自洽；`[hot]` 诊断。验证：nested 在 count=11、plain 在 count=51 精确触发，25/25 全绿（阈值只影响短跑/冷启动，稳态不变）。clang：driver 本就是路径参数，flags 全兼容，无需改代码，本机无 clang 实测待补。三级编译暂不做（等 ELS 落地后评估差距）。**技术债清单（8 项）**：① 单例 state 代码累积无上限 ② ELS 未落地 ③ vr-inline 休眠 ④ 热点再编译机制 ⑤ 分配器（最大单项）⑥ memo 只按 self 读法 ⑦ 裸 driver 名诊断弱 ⑧ 重复符号名的 longjmp 行为待确认。见 current.md <71>

<72>(SOLVED + SOLVED): **多 Runtime 安全确认 + struct free list**。审计：单例 state 有守卫不重复 `tcc_new`；vr_expect/host 符号/seq 文件名全进程安全；约束是**多线程并行编译不安全**（生成器暂存进程全局），同线程交替安全——实测 `multi_rt` 双 Runtime 全对。free list：`JSRuntime` 按宽度分桶（`p->prop` 串链，<64 深，`in_free` 守卫 + FreeRuntime 排空），回链跳过 shape/prop/finalizer，取用重挂 GC 链表 + 释旧列 + 全列重写。两个 bug：复用对象不在 GC 链表（列被回收，启动期崩）、旧列值泄漏。immutable 339→245ms（-28%），objshape -9%（escaped 仍是 GC 扫描主导）。见 current.md <72>

<73>(SOLVED + 测量): **ELS 上限实测 + c1 死变量 + pe_isafunc 真 bug**。gcc-O2 同函数 1.7-6x（nested 6x/locals 4.5x/minmax 4.7x/arith 1.8x/bitops 1.7x）——大头是 gcc 全局优化，ELS 纯分配稳拿 1.5-2x。bug#1：fast path 的 `c1` 从未被赋值（全文件无写入），常量传播从没工作过 → 改 `(k1==WLD_INT||WLD_REAL)`；`lo_c` 提 static + `local_inline` 回退发字面量。bug#2：`pe_isafunc` 用 `ELF32_R_INFO` 在 x86_64 恒假 → 不建 thunk jmp → call 跳 IAT（fibg 靠运气，bits 的 SIGFPE 暴露）→ 改 `ELFW(R_INFO)(R_X86_64_PC32)`。删 `whale_const` 死代码（与 fast path 重叠）。LICM 未动手（`loop_end` 插入点已确认，需 token 解析，下轮）。见 current.md <73>

<74>(SOLVED + 危险): **`set_loc` 漏统计 → 常量传播静默错答案**。`mixed`（`x=1.0` 初始化 + 循环内 `x=x+y*2`）JIT 给 `60000005`，正确是 NaN（指数发散后 `Inf-Inf`）——`x` 被误判单赋值常量 `1` 重写循环体。根因：`assign_cnt` 只计 `put_loc/inc_dec`，不计 `set_loc`（存值又留栈）；与 <13> 的 `inc_loc` 漏统计同类。修法：预扫描加 `set_loc` 全形态。教训：任何写局部的 opcode 都必须进 `assign_cnt`。见 current.md <73 bug#3>

<75>(SOLVED/部分): **peephole 常量折叠 + live 拒绝的常量例外**。双字面量 `+-*/` 编译期 double 求值（IEEE 一致，无 int 陷阱；小整数发 `wt_int` + 可继续传播）。`live` 拒绝让 peephole 对含常量表达式永不匹配 → 常量局部不拒绝；`local_inline` 对 flush 后常量发字面量。nested 90→73ms，其余持平。`(L0+L1)` 未折叠是 deferred 变量转发，需 `local_set` 透传 `lo_c`（下轮）。见 current.md <74>

<76>(TIPS+未解): **fib 退化排查 + memo 关断回退 + A 收尾评估**。`by_flag` 死计数器删除（保留 `whale_entry_total`）。memo 关断（`pure1&&!selfref`）致 fib 16→349ms + deopt 暴涨，已回退，原因未明（疑 entry 选择与 memo 隐性交互）。A 收尾做不了：`wt_slot` 是单 ident token，生成器侧无 ident 表，无法反查变量号 → 排到 ELS（线性 IR 自带符号表）之后。见 current.md <76>

<77>(PARTIAL): **LICM 地基（inv 追踪 + 外提框架，未生效）**。`WhaleVExpr.inv`（字面量/参数置 1，binop 取 AND，其余 0）；`fold_materialize` 在循环内 + inv 时发外层输出（只做栈槽）。语义安全（默认不外提）。未生效：nested 的 `_ws.v` 没外提，`inv` 链断在 `(L0+L1)`（peephole 没匹配 → 通用路径不记 inv）→ 下轮加 inv 诊断找断链点。见 current.md <77>

<80>(SOLVED/教训): **inv 继承灾难（11 case 静默错答案）**。`fold_set` 的 `inv` 继承（`whale_fold_src`）让 `call`/helper 结果继承参数源的 `inv` → 调用被搬出循环 → 11/25 case 全错（多为 `check=0`）。修法：删继承，`inv` 只由构造点置位（字面量/参数/binop AND/`lo_c`）。教训：`inv` 只能构造点置位，绝不能转发继承（转发不知道目标语义）。附带：runner entries 列部分空白（功能正常，待查）。见 current.md <80>

<78>(SOLVED×2+TIPS): **CStruct 频率化 + object free list + 线性表结论**。`whale_sites_freq` 接受 ≤4 形状 site；布局按**去重形状覆盖率**≥40%（gcc 二编降到 0 全量），按覆盖率降序截断 16。中间踩坑：按 site 数算把 objfields 干到 188ms（tag 占 2/10 site 被滤）→ 改形状级后恢复 63ms。object free list（`ObjNew` 空对象单链，`DefineNum` 照常生长）：objshape -8%、immutable -17%。线性表结论：`col_lookup` 热路径零调用（缓存全命中）→ **保持线性，不引入哈希**。见 current.md <78>

<79>(SOLVED×2): **布局去上限 + object free list 全复用**。16 只留作 free list 分桶数；布局/栈数组/prologue/`StructNew`/`Native` 全改 32（prologue 固定全宽声明，因布局在 emit 中期才定）。object free list 第一版只收空对象 → `hit=0`（escaped 死时已有属性）；改收非空（释值置 UNDEFINED，链存 `u.opaque`，shape/prop 保留，accessor 另走）→ `hit=100%`。immutable 波动是 GC 噪声。见 current.md <79>

<81>(SOLVED): **LICM 转正（inv 链修通 + 外提生效）**。三处断链：① 变量读 inv 恒 0（含 lo_c 字面量路径之外的所有变量）→ 加 `whale_loop_written`（每循环头扫 [head,back] 的写 local 位集）+ `whale_local_invariant`，变量读/get_loc0_loc1/peephole-1 的 SLOT 操作数在体内无写时 inv=1；② `fold_set` 内超长 materialize 读到回写前的 0（nested 的 div 实测）→ `fold_set(k,mark,inv)` 改传参（25 调用点全改，另加 `whale_load_inv` helper 去重）；③ 单目/vr-inline/get_loc0_loc1 的显式回写并入参数。内存读与 call 结果（vmat=1 的 10 处 + struct 列读）显式清 0（防 stale 继承）。`local_set` 加 inv 参数（put/set_loc 传栈顶 inv，add/inc_loc 传 0）。效果：nested 88-94→70-74ms（外提 2 次，check 对），25/25 全对 0 mismatch。见 current.md <81>

<82>(PARTIAL): **ELS 机制恢复 + 范围收缩（local pin hang 待查）**。`WHALE_EXPR_MAX` 24→200 探针证伪"语句合并"方向（nested/minmax 反而慢 14-29%，只有 bitops 快 16%）；TCC 语句边界 spill 是架构性的（save_regs/vstore 全是语义必需，无小改）。恢复 pin 机制（8 处：alloc+偏移表+Enter/Leave、save_reg、get_reg×2、gv 类检查+重载、gen_op 归一化、vstore、TOK_REGISTER、decl 分配、gen_function 重置）+ 生成器侧 ELS 决策（loop heat 选 top-4、排除对象/lo_c 单赋值、纯数值白名单 `whale_is_pure_num_op`）。修好三个真 bug：① Invalidate 放错 gfunc_call 版本（ELF 版而非 PE 版，bitops/stringcat 错答案）；② save_reg_upstack 无条件清 validity bit（`?:` 的非调用 save 也清，pin 全失效）；③ 跨函数 pin 表误匹配。当前范围：local pin 全关（nested/minmax 任 pin 一个 local 即 hang：归纳变量不前进 + 比较操作数错乱，hang.asm 为证，下轮专项），_wv pin 只在无调用函数开。效果：25/25 全对 0 mismatch，零收益零回归（nested 76、bitops 154、array 53 全是基线噪声内）。见 current.md <82>
