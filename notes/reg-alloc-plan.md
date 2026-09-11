# 寄存器保留方案（TCC x86_64 后端）

目标：让循环里的局部变量跨语句住在寄存器里。现状是**每次读都 `load`、每次写都 `store`** —— `SValue.r & VT_LVAL` 一出现，`gv()` 必然去取内存。

参考实现：8cc / 9cc / chibicc 一类轻量后端的寄存器处理（用户指定）。

## 接口：复用 `register` 关键字

- `tcctok.h:27` 有 `DEF(TOK_REGISTER, "register")`
- `tccgen.c:4121` 的 `case TOK_REGISTER:` 目前只 `next()` —— **完全忽略**
- **没有任何 `VT_REGISTER`**（已用 `-Recurse` 复核；注意 `Get-ChildItem -Include` 不带 `-Recurse` 是假阴性，我在这上面错过一次）

所以接口现成：`whale_jit.c` 把该固定寄存器的变量声明成 `register double x;`，TCC 侧让 `register` 真正生效。`whale_tcc_iface.c:610` 的关键字白名单里也已经收了 `TOK_REGISTER`。

## 改动点（8 处，跨 3 文件）

| # | 位置 | 改动 |
|---|---|---|
| 1 | `tccgen.c:4121` `case TOK_REGISTER:` | 不再忽略：置一个 `pin_next` 标志（`TOK_AUTO` 等保持忽略） |
| 2 | `tccgen.c` 建局部 `Sym` 处 | `pin_next` 时从一个**保留寄存器池**取一个，记进符号，然后清标志 |
| 3 | `tccgen.c` `get_reg()` / `get_reg_ex()` | 跳过保留池里的寄存器（两处循环：选空闲的、以及没有空闲时 spill 的那个）—— 否则固定寄存器会被分配出去 |
| 4 | `tccgen.c` `gv()` (~1326) | 该变量已有固定寄存器时**跳过 `get_reg` + `load`**，直接返回该寄存器，并清掉 `VT_LVAL` |
| 5 | `tccgen.c` `vstore()` (3069) | 目标是固定寄存器变量时**不 store** |
| 6 | `tccgen.c` `save_reg_upstack()` (969) | 跳过固定寄存器，别把它 spill 掉 |
| 7 | `tcc.h` `Sym` (466) | 存"该符号的固定寄存器号"。`SymAttr` 的 16 位已满（只剩 `unused:5`），**加独立字段更干净**，但要确认 `sym_push2` 是否零初始化（notes `<23>`：`sym_push2` 连 `CType.ref` 都不复制） |
| 8 | `whale_jit.c` | 对选定变量发 `TOK_REGISTER`（`whale_keywords` 里加 `JS_WK_REGISTER`） |

**必须一起做完**：只做 1–3 会让变量被分配了专属寄存器、而 `gv`/`vstore` 仍按内存操作 → 直接生成错误代码。

## 关键约束

- **函数调用会破坏寄存器。** 初版只对**函数体内没有 `call`** 的函数启用（`whale_jit.c` 侧可判定，那里已经有循环缓冲 `loop_begin`/`loop_end`，循环体是完整的，选哪些变量固定是纯编译期决定）。
- 调用者的寄存器保存约定、`nocode_wanted`、`VT_MUSTBOUND` 等路径都要照顾到，别在 `nocode_wanted` 下分配。
- `SValue.r` 是 `unsigned short` 且 0x0100–0x8000 全被 `VT_LVAL*`/`VT_SYM`/`VT_MUSTCAST`/`VT_MUSTBOUND`/`VT_BOUNDED` 占满 —— **不要指望在 `r` 里腾新标志位**。
- x86_64 的 `NB_REGS = 25`（`NB_ASM_REGS = 16`），保留池要避开 `gen_opi()`/`gen_opf()` 内部会硬用的寄存器（`get_reg` 的注释特别提醒 spill 要从栈底开始，就是为了这个）。

## 第一次尝试的结果（重要）

TCC 侧已落地（commit `eb890d5`：`whale_pin_alloc` + `get_reg` 保留 + `gv`/`vstore`/`save_reg_upstack` 特判；无调用者，全量 0 mismatch）。

生成器侧接上后（对 `!whale_has_call` 的函数，所有局部变量声明为 `register`）**收益确认，但正确性错了，已回退**：

| case | 之前 | pin 后 | check |
|---|---|---|---|
| `arith` | 111ms | **65ms** | **错**：`727740513040600.2` → `20000000` |
| `locals` | 101ms | **54ms** | 对（`Infinity`） |
| `loop_sum` | 60ms | **57ms** | 对 |

**快了 ~45–50%，正是 `<19>` 预估的上限**，方向没问题 —— 但 `arith` 算错，且某个 case（`bitops` 之后）**挂住**。

**第二次尝试（已修标志位）—— 错误不变**，所以 bug 不在标志位。但拿到了关键诊断：

- 生成代码是 `register double L0 = 0; register double L1 = 0;` 然后 `L4: if (i < n) … L0 = ((L0 * A1) + A2); L1 = (L1 + 1); goto L4; return L0;` —— **看起来完全正确，且 `L0`/`L1` 全内联（没有 `v<n>` 物化），这解释了 45% 的提速**。
- **`arith` 的结果是 `20000000`，而 `n = 20000000`** —— 正确的是 `7.27e14`。返回值等于循环次数，**意味着 `return L0` 拿到的是 `i` 的值** → **`L0` 和 `L1` 落到了同一个物理寄存器**（`L1` 覆盖了 `L0`）。
- `loop_sum`（`s += i` 之类）反而是对的，`arith`（`s = s * c + k`）错 —— 差别在算术复杂度，不在有无 pin。

**因此下次的第一嫌疑改为：`whale_pin_alloc()` 的分配去重没生效。** 具体要查：
- `whale_pinned_regs` 是否真的在两次分配之间**保持**（它是 `static`，但 `tcc_new` 会不会重置、或者每次编译是否要清零？**没清零** → 多轮编译会累积，但一轮内应该正常）；
- `reg_classes[r] & RC_FLOAT` 对 22/23（xmm6/xmm7，**明确不含 `RC_FLOAT`**）应被跳过，21/20 应依次返回；
- **`sym->pin_reg = pr + 1` 写进的是不是 `gv()` 后来看到的那个 `Sym`** —— 在 `decl()` 里 `sym_push` 之后写，但 `unary()` 求值标识符时挂到 `vtop->sym` 的可能是**另一个** `Sym`（`sym_push` 会同时挂进 `local_stack` 和 token 表）。
- 最快的二分：**只 pin 一个变量**（例如只对 `L1` 发 `TOK_REGISTER`）。若结果正确，问题就在"两个变量拿到同一寄存器"。

**下次的排查线索**（其余，按可能性排序）：
1. **`gv()` 的 pin 分支直接 `vtop->r = pin_reg; return;`** 是**整体赋值**，把 `vtop->r` 上所有别的标志位一起清掉了（`VT_SYM`、`VT_MUSTBOUND`、`VT_MUSTCAST` …）。该保留它们、只换位置位：`vtop->r = (vtop->r & ~(VT_VALMASK | VT_LVAL)) | pr;`
2. **`vtop->sym` 对局部变量求值未必非 NULL、也未必是*那个*符号** —— 其语义是 "if (VT_SYM | VT_CONST), or if result of unary() for an identifier"。若指向别的 `Sym`，pin 就用错寄存器。**先验证**（在 `gv` 里打印 `vtop->sym->v` 与 `pin_reg`）。
3. **`vstore()` 的 pin 分支手写 `vswap(); vtop--;`** 模仿正常收尾，但正常路径还带 `delayed_cast` 与 `vtop->r |= ...`；且 `gv(rc)` 可能因 `vtop->r` 恰好等于 `pr` 而**误判"值已在 pr"**（类匹配只看 `reg_classes[r] & rc`）→ 根本不 load，值就是陈的。
4. **`whale_has_call` 只认 `OP_call0..3`/`OP_call`** —— 若还有别的 opcode 间接调 JS（`OP_call_constructor` 已单独处理，`sieve` 走那条），要一并算 clobber。
5. **初版很粗**：把**所有**局部变量都声明成 `register`（靠 `whale_pin_alloc` 的 `is_float` 挡非浮点、靠 6 个浮点寄存器上限挡超额）。更稳的是只 pin **真正的循环归纳变量** —— `loop_begin`/`loop_end` 能拿到完整循环体，本来就是为此准备的。

## 验证

1. **先不用 JIT**：写一个 `register double` 的小 C 文件，走 `whale_tcc_compile` 的 C 源路径，确认语义正确（这是唯一不依赖 `whale_jit.c` 的隔离验证）。
2. 再让 `whale_jit.c` 对循环变量发 `TOK_REGISTER`，跑 `node perftest/main.js --engines=node,whalejs`，`check` 必须跨引擎一致。
3. 性能看 `arith`/`locals`/`loop_sum`/`bitops`/`minmax` 这类纯数值循环（现状 1.7×–3.4×）。notes `<19>` 预估寄存器保留 +20~50%。
4. 回归重点：带 `call` 的函数（`fib`）、`sieve`（走对象槽那条路）。

## 另一条已被否掉的路

在 TCC 单遍流式发码的现状下做**循环级**优化（把归纳变量固定在寄存器）**必须能看到控制流**。notes `<19>` 的结论是"必须 IR 直出通了才动得了"—— 但 `<34>` 之后走的是 **token 通路**（生成器发 token 给 TCC parser），IR 直出并没做。所以循环优化仍然只能在 `whale_jit.c` 侧做（它有完整的循环体），**而"固定寄存器"是 TCC 侧唯一缺的那块**，这正是本方案要补的。
