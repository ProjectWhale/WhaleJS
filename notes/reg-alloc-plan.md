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

## 第三次尝试（gv 的类检查修复）—— arith 修好了，locals 还不行

**真 bug 找到了**：`gv()` 会被用**特定寄存器**调用 —— 返回值的 `RC_IRET`/`RC_FRET` 要求值落在返回寄存器里。原来的 pin 分支**无条件**返回 pin 寄存器，跳过了 `reg_classes[pr] & rc` 的类检查，于是 `return s` 读到的是一个值从未在其中的寄存器 —— 这就是"`arith` 返回循环计数"的全部原因。修法是：类匹配就直接用，不匹配就 **`get_reg(rc)` + `move_reg`** 把值搬出来（**不能走 `load`**：pin 变量从不存内存）。

修完后：

| 测试 | 结果 |
|---|---|
| `c=2,k=3,n=20` | **3145725 ✓** |
| `c=3,n=5` | **243 ✓** |
| `arith` perftest | **check=727740513040600.2 ✓，111ms → 62ms** |
| `locals` perftest | **仍错**：`13333337`（node 是 `Infinity`） |

**`locals` 的最小复现**：`function r(n){var a=1,b=2,c=3; for(var i=0;i<n;i++){var t=a+b; a=b+1; b=c+1; c=t;} return a+b+c;}` —— `r(20)` 给 **17**，node 给 **3388**。

**生成的代码完全正确**（`register double L0..L4`，5 个变量分到 xmm5..xmm1）：
```c
L0 = 1; L1 = 2; L2 = 3; L3 = 0;
L8:  if (!(L3 < A0)) goto L32;
     L4 = L0 + L1;   L0 = L1 + 1;   L1 = L2 + 1;   L2 = L4;   L3 = L3 + 1;
     goto L8;
L32: return ((L0 + L1) + L2);
```

**所以这一次的 bug 与 pin 的*数量*相关**：`arith` pin 2 个变量（xmm5/xmm4）正确，`locals` pin 5 个（xmm5..xmm1）错误。而 `whale_pin_alloc` 从高往低取，特意留下 xmm0 给 TCC 自用，5 个也还在 6 个浮点寄存器的池内。

**下次先做的实验**（每次构建 ~17s，很便宜）：
1. **pin 数量二分**：只对 `i < 3` / `i < 4` 发 `TOK_REGISTER`，找出从几个开始错。若 4 个就错，问题在"多于池子的安全余量"，而不是分配去重。
2. **怀疑 `get_reg(rc)` 在 `gv` 里的副作用** —— 它自己的注释就写着 "NOTE: get_reg can modify vstack[]"，而对于一个 pin 变量我是在 `gv` 中途调用它的。
3. **怀疑 `gen_op` 的就地修改**：`gen_op` 会把结果算进**左操作数**的寄存器（`dst = vtop[-1].r`），而 pin 变量正好常做左操作数 → 它的寄存器被就地改写。逐条看时每一步都"马上会被覆盖"所以没错，但多变量交织时未必。
4. `move_reg` 里 `save_reg(r)` 依赖我让 `save_reg_upstack` 跳过 pin 寄存器；确认它没有在别处被绕开。

## 第四次尝试（栈偏移反查）—— 更接近了，但还差一层

**这一轮修掉的两个真问题**：
1. **`vstore` 用 `vtop[-1].sym` 判断目标是不可靠的** —— `unary()` 只在*读*标识符时设 `sym`，而**声明的初始化**没设。两边的判断因此不一致：初始化把值写进**栈槽**，`gv` 后来却去读**寄存器**。机器码直接给出了证据（`movq [rbp-8], xmm0` —— 本该是 `movsd xmm5, xmm0`）。改为**用栈偏移反查**（`whale_pin_loc[]`/`whale_pin_reg[]` + `whale_pin_find()`），不再依赖 `sym`。修完后初始化确实进了 xmm5/xmm4/xmm3。
2. **`whale_pinned_regs`/`whale_pin_n` 跨函数不重置** —— `gen_function()` 现在每个函数开头清零（否则上一个函数的 偏移→寄存器 映射会误匹配本函数的局部）。

**剩下的根因（已用机器码 + 诊断双重确认）**：`gen_opf`（`x86_64-gen.c:2020`）靠 `vtop[-1].r & VT_LVAL` 来决定"t1 是寄存器、t2 是内存引用"，并据此 `vswap`。pin 让 `gv()` **提前清掉 `VT_LVAL`**（值已在寄存器），于是 `gen_opf` 的判断与实际发出的指令**分叉**：

- 循环体机器码：`addsd xmm0, xmm5`（结果落 **xmm0**）
- 而 `vstore` 里 `gv()` 返回的是 **21**（pin 寄存器 xmm5）→ `r == pr` → **不发出写回**
- 三个赋值互相覆盖，`a`/`b` 从未被更新 → `r(3)` 得 3 而不是 9

**下一次的正确方向**（不是再小修）：让 `gen_opf`/`gen_opi` **知道 pin 寄存器不能被当作结果落点** —— 即 `gv()` 对 pin 变量返回寄存器时，要么保证调用方接受"值不在它以为的寄存器里"，要么在 `gen_op` 层面把 pin 变量当作**只读操作数**、结果一律落到普通寄存器。现在 `arith`（`s = s*c+k`，操作数都是内存里的参数）能正确且快 45%，而只要**pin 变量的寄存器与常量/临时混在一个表达式里**就会错 —— 那正是 `a = a + 1` 的形状。
## 第五次尝试（gen_op 归一化）—— 通了

**关键修复**：`gen_op()` 入口把 pin 操作数标成"已在寄存器"（`r` 换成 pin 寄存器号、清 `VT_LVAL`），再交给 `gen_opf`/`gen_opi`。那两个函数靠 `VT_LVAL` 判断"哪个操作数是内存引用"，而 pin 变量的值**不在内存** —— 把它当内存引用就会 `addsd dst, [栈槽]` 读到从未写过的旧值（`return a + b` 读到过期的 `b`）。

**外加两条护栏**：
- **最多 pin 4 个浮点寄存器**（`whale_pin_alloc` 从 21 只降到 18）—— 留 xmm0/xmm1 给 TCC 放常量和中间结果。pin 满 6 个会让 `get_reg()` 无处可 spill 而返回 **-1**。
- **跳过嵌套循环**（回边数 > 1 就不 pin）—— 两个归纳变量都 pin 会让 `minmax` **死循环**（`j` 永远不前进）。

**结果（全量 0 mismatch，所有 case 跑完）**：

| case | 前 | 后 |
|---|---|---|
| `bitops` | 185 | **170** |
| `minmax` | 126 | **106** |
| `nested` | 101 | **92** |
| `sieve` | 184 | **173** |
| `branch` | 124 | **116** |
| `array` | 185 | **176** |
| `loop_sum` | 59 | **55** |
| `mixed` | 207 | **198** |
| `locals` | 110 | 104 |
| `arith` | 114 | 115（持平） |

**收益 5–12%，低于 notes `<19>` 预估的 20–50%，原因是决定性的**：生成器已经把整条表达式融合成**一条语句**，所以每个变量每轮本来就只有**一次 load/store** —— pin 省的正是那一次。**语句数才是大头**，pin 已经把"每语句一次内存往返"这个上限吃掉了。下一步该回到**减少语句数**（`fold_materialize` 由表达式超长触发，见本文档前面的分析）。
## 语句数方向：两个被实测否掉的尝试（重要）

**1）`WHALE_T_ICONST`（字面量常量的 ToInt32 直接退化成 `(int)常量`）—— 反而变慢。**
想法本身对：`bitops` 里 `ToInt32(255)`/`ToInt32(3)`/`ToInt32(1)`/`ToInt32(0xffff)`/`ToInt32(15)` 全是**编译期常量**，每个却生成 4 层 `?:` 范围测试（约 15 token）。但实现后 `bitops` **151 → 172ms**。

**原因是一个关键权衡**：表达式变短后**不再超过 `WHALE_EXPR_MAX`，于是不再物化**。而 `v0 = i & 255` 在 `bitops` 里被引用 **4 次** —— 物化一次再引用 4 次（每条发一个 `v0`，1 token）远比内联 4 次（每次约 10 token）便宜。**所以"缩短表达式"不是无条件的好，它会拆掉物化的触发条件。**

**2）门槛本身的扫描**：`WHALE_EXPR_MAX` 试了 12 / 24 / 36：

| 值 | `bitops` | `ident` |
|---|---|---|
| 12 | 186 | 81 |
| **24** | **156** | **67** |
| 36 | 164 | 65 |

**12 明显差**（物化太频繁 → 语句太多），36 与 24 在噪声内。**24 接近最优，保持不动。**

**下一步的方向因此要改**：不是"让表达式更短"，而是**让物化决策看见"这个值会被引用几次"**。单遍生成器只有 `whale_load_analyze` 那点前瞻，所以可行的做法是**在 `fold_set` 里按已见到的重复使用次数延长/缩短门槛**，或者干脆**在 `loop_begin`/`loop_end` 拿到完整循环体后做一遍引用计数再决定谁物化** —— 后者本来就该是循环缓冲的用途。