# 原生控制流编译 — 准备（for / if / while / switch）

Status: **PLAN — 未开始实现**。本文件是下一轮改动的落点调研与 bug 预估，不含代码改动。

## 0. 为什么做这个

`objfields` 与同一循环体、仅把字段换成局部变量的版本（`temp/w_of_split.js`）实测几乎相同：

    whalejs  OF 16/32/57/72ms   LOC 15/35/51/61ms   (~16 ns/iter)
    node     OF  4/10/15/16ms   LOC  3/7/10/22ms    (~4  ns/iter)

结论：**对象字段访问不是瓶颈**；whalejs 的循环体整体比 node 慢 ~4x。同文件也推翻了旧笔记里
"D 纯局部 3ms vs C 对象字段 70ms 23x" 的说法 —— 那次两边的循环体不同，差的是取模不是对象。

`%` 的守卫**不是**本次目标：`fold_mod` 已用整数 `idiv`，且 `whale_jit.c:4519-4523` 的项目自测写着
`guarded idiv 1776ms, bare idiv 1781ms, fmod 3028ms / 2e8 ops — The guard is free`。
削守卫无收益，本次不动。

真正要动的：**循环本身**。现在的循环是字节码 goto 的 1:1 C 翻译。

## 1. 现状：控制流怎么生成的

QuickJS 没有 `OP_for` / `OP_while`；for/while/do 在字节码层就是 `goto` + `if_false`。
所以"for 原生编译" = **在 goto/if 的图上识别循环形状，改生成 C 的 `for`**。

### 1.1 scan pass（第一个 pass，`whale_jit.c` ~1897 起）

- 1914-1920：识别 `goto8/if_false8/if_true8`（1 字节偏移）与 `goto/if_false/if_true`（4 字节）。
- 每个跳转目标第一次到达时，把当前类型向量 `cur[]` **快照**进 `snap + tgt*state_len`，并置 `seen[tgt]`。
- 快照只做一次（`!seen[tgt]`）。**这是循环类型收敛的现有机制，也是最大的已知限制**（见 §3.1）。

### 1.2 emit pass

- `OP_if_*`（4677-4698）：发 `if (!(cond)) goto L<tgt>;`，然后若目标未 seen 则快照。
- `OP_goto*`（4700-4715）：发 `goto L<tgt>;`，置 `unreachable = 1`，然后快照。
- `OP_return`（4716+）：发 `return ...;`，`unreachable = 1`。
- 标签 `L<n>` 由 `wt_slot("L", n)` 生成（`n` 是字节码位置）。
- `whale_step_is_control`（1607-1621）把 `goto/if_*` 以及 `call*`/`return`/`throw` 列为 control，
  使 `whale_step_emit` 拒绝它们 —— 这些必须由主生成器处理。

### 1.3 类型状态

- `cur[var_count + sp]`：操作数栈顶类型；`cur[0..var_count-1]`：局部变量类型。
- `snap[...]`：跳转目标的类型快照；`state_len = var_count + stack_size + 1`（末槽是 `sp`）。
- `unreachable`：goto/return 之后的字节码不再生成（DCE，见 index `<13>`）。

## 2. 哪里能改（落点清单）

### 2.1 循环识别：已由编译期提供（2025 实施）

**不再**在 JIT 侧做流式预扫描。改为**编译期收集 + attach**：

- `WhaleLoop { int head; int back; int exit; uint8_t kind; }`，`WHALE_LOOP_MAX 64`。
- `compute_stack_size`（`quickjs.c` ~39021-39044）在它本就要算跳转目标的
  `goto*` / `if_*` 分支里调 `whale_note_loop(fd, pos, tgt, cond)`：
  `tgt < pos` 即回边 —— **回边是确定性的，不是从 opcode 反推**。
- `js_create_function` 把 `fd->whale_loops` memcpy 进 `JSFunctionBytecode`（内联数组，
  不进 GC、不进克隆路径、无需 free；超 64 个不记，生成器对这些回退 goto）。
- `WHALE_LOOP_DBG=1` 打印。已实测：`sieve` 4 个（含嵌套）、其余用例各 1 个，
  全部 `cond=0`（无条件回边，正是 for/while 形态），0 mismatches，性能与基线一致。

`kind` 区分向后 `if`（do/while 底部测试）与向后 `goto`（for(;;)/while 回边）。
`exit` 目前留 -1，是给 `break` 用的（head 处 `if_false` 的目标），下一轮补。

识别规则（从简单到复杂，按收益/风险排序）：

1. **canonical `while`/`for` 形态**（先做这个）
   - `head`: 某 `if_false`（或 `if_true`）跳到 `exit`，且 `exit > head`
   - `body`: `[head+sz, back)` 的直线代码
   - `back`: 一个 `goto head`，且 `back > head`
   - 条件操作数只依赖在 `head` 处已知类型的槽
2. `do { } while (c)`：`goto` 在**后面**指向前面（back edge 在前，head 在后）
3. `for(;;)` + 内部 `if` break：多出口，退化到现有 goto 路径
4. `switch`：QuickJS 编译成 `if` 链或 `OP_goto` 表；**不建议**本轮做（收益低、面大）

### 2.2 生成本身

在 emit pass 的 `OP_if_*`/`OP_goto*` 分支（4677/4700）前面加一层判断：

- 若当前 `pos` 是一个已识别循环的 `head` → 改发 `for (;;) { if (!(cond)) break; ... }`
  或更贴合的 `for (init; cond; incr)`（`init` 已在循环前，`incr` = 回边前的 `i = i + 1`）
- 若 `pos` 是回边 `goto` → **不**发 `goto`；若这是 `for` 的 incr 部分，把它挪到 `for` 的第三段
- 其余跳转保持现状不动

**关键约束**：`fold_materialize` 与 `fold_commit` 只在**语句边界**安全（它 append 的是语句）。
循环改写必须发生在语句边界，不能塞进正在构建的表达式里。参见 `OP_mod`（4506-4509）的做法：
物化放在 `fold_mark()` **之前**，注释里明确写了原因。

### 2.3 变量与设备

- 循环计数器：JS 语义是 double，生成 C 的 `double i`（不要用 int，`i++` 溢出 int32 会与 JS 不同）。
  已有机制：局部变量就是 `L<i>`（`wk(JS_WK_DOUBLE)` + `wt_slot("L", i)`，prologue 2359-2363）。
- 循环里出现的新 `_ws.v[nm]`：`whale_vserial` 在扫描期分配，循环体只生成**一次**，所以名字不会因
  循环而冲突 —— 但若将来做循环展开，这一点会立刻变成 bug 源（见 §3.4）。
- `_ws` 快照：现在每个汇合点一份（`fver`）。循环回边是隐式汇合，需要确认 `fver`/字段缓存在
  回边处的有效性。

## 3. 预估 bug（按可能性排序）

### 3.1 循环内类型变化 → 快照只做一次

`seen[tgt]` 只快照一次，第二次到达同一位置时类型若不同，**不会**被重新检查。
今天靠"每个函数只编译一次、且类型在 grep 到的访问点上是单态的"侥幸成立。
一旦循环体让某个槽的类型在迭代间变化（例如 `x = 0` 后 `x = "s"`），
`snap` 就是错的，生成代码静默算错。
**对策**：循环识别时要求**循环体内被写的槽，其类型在 body 内不改变**，否则拒绝该循环。

### 3.2 deopt 在循环内 → 副作用重复

`*deopt = 1; return 0;` 会让整个函数回到解释器重跑。若循环体里已经执行过有副作用的操作
（DOM 写、`print`），重跑会重复它们。项目里 `JS_WhaleCallValue` 的注释已提到这一点
（"A DOM mutation must not happen twice"）。
**对策**：含 call 的循环体不做原生改写，或只在"循环外无副作用"可证时改写。

### 3.3 `sp` 与栈形状不一致

`sp` 存在 `cur[state_len - 1]`，快照包含它。若循环体内部的临时弹栈/压栈不守恒，
改写成 C 的 `for` 后编译器**不会**帮你对齐栈 —— 而原来的 `goto` 结构恰好靠标签把状态钉住了。
**对策**：识别时校验 `head` 与 `back` 的 `sp` 相同，且 body 内 `sp` 单调不越界。

### 3.4 `unreachable` 与 DCE 交互

`goto` 现在会置 `unreachable = 1` 并吞掉后续代码。改写后回边不再是 `goto`，
`unreachable` 的置位点会变，可能让原本被 DCE 掉的代码重新生成（体积变大）或反过来吞掉 `for` 的第三段。
**对策**：改写只影响回边与 head 两处，`unreachable` 的语义保持"紧跟在 return/goto 之后"。

### 3.5 嵌套循环与 `break`/`continue`

`break`/`continue` 也是 `goto`。若外层被改成 `for` 而内层 `break` 跳到外层出口，
C 的 `break` 语义会指到最近一层。
**对策**：本轮只处理**最内层、单出口、单回边**的循环；含 `break`/`continue` 的循环继续走 goto。

### 3.6 标签仍然需要

即使循环本体改成 `for`，循环**外**仍可能有 `goto` 指向 `head` 或 `exit`（如前面的代码）。
若删掉 `L<head>:` 标签而仍有引用，TCC 会报 `label not defined`（这类错误在 index `<56>` 附近
有先例：`exc 标签丢失`）。
**对策**：标签的发射与引用计数解耦 —— 只发被引用到的标签，或先收集再决定。

## 4. 验证计划（每步都要跑）

1. 每改一处：`xmake build whalejs`，**先只跑 `object`**（它必须保持 4-6ms，是回归哨兵）。
2. 再跑 `objfields` / `bitops`，各取 3 次最小值。
3. **全量 24 个 perftest 对 node 对照，要求 0 mismatches**。
4. 必须同时跑**原版对照解释器** `build\mingw\x86_64\release\qjs.exe`：
   whalejs 的时间若贴近原版 qjs，说明**根本没编译成功**（index `<55>` 的教训：
   "slow" 不是 JIT 跑过的证据）。
5. `WHALE_JIT_DUMP=1` 看 `bail=` / `gen gave up`；`WHALE_JIT_SRC` dump 生成 C 确认语法合法。
6. 每步做完立即 build 再 commit（index `<56>` 附近记过"提交过三次编译不过的状态"）。

## 5. 建议的推进顺序

1. **循环识别预扫描**只做识别、不改生成，用 `WHALE_LOOP_DBG` 打印识别到的循环，
   在 24 个用例上核对有没有误判（这一步零风险，纯观测）。
2. canonical `for`/`while` 改写，含 `break`/`continue` 的循环一律跳过。
3. 逐用例放开条件，每次全量对照。
4. 再考虑 `do-while`、嵌套；`switch` 单独立项。

## 6. 相关文件

- `src/whale_jit.c`：1607（control 判定）、1897-1930（scan 跳转快照）、2732（快照判定）、
  4677-4715（if/goto emit）、2352-2363（局部变量声明 + pin）。
- `notes/pinned-registers.md`、`notes/pinning-design.md`：寄存器/`xmm` 的代价（caller-saved）。
- `notes/reg-alloc-plan.md`：语句数与寄存器保留的历史结论。
- `temp/w_of_split.js`：本轮分解脚本（不入 git）。
