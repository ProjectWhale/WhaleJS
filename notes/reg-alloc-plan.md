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
## 当前边界（实测）

两个最慢的 case **完全没有进 JIT**，跑的是解释器：

| case | 放弃点 | 需要的支持 |
|---|---|---|
| `object`（404ms，node 25ms） | `pos=0 op=11 (object)` —— 对象字面量 | `OP_object` + `define_field`/`put_field`/`get_field`，且**属性访问必须内联缓存** |
| `array`（189ms，node 19ms） | `pos=0 op=38 (array_from)`、`pos=6 op=239 (get_length)` | 数组字面量/增长 + `length` |

**`object` 的形状最有希望**：`o = { a: 0, b: 0, c: 0 }` 是**局部字面量**，形状编译期已知，属性都是数字 —— 与 TypedArray 那条路同类。**但和 TypedArray 一样，第一版走 helper 一定会慢过解释器**（每属性一次 C 调用，TypedArray 第一版就是慢 18 倍），所以必须直接做**内联缓存**：为每个 (对象槽, atom) 缓存属性值的 `JSValue *`，生成代码直接读写它。

**要多做一层的是数值域**：QuickJS 的属性值可能是 int32 tag 也可能已解引用（`JSVAL_REF`），所以读写都要 helper 做装箱/解箱，或者缓存里再带一个"已知是 double 域"的标志。

**`array` 属于同一族**（数组特化 + 长度缓存），但 `scan(a)` 的 `a` 是**参数**、形状不可静态确定，比 `object` 难。

**建议顺序**：`object` → `array`。两者都远比"减少语句数"实在（那是 14× 和 10× 的缺口，而语句数方向已经被证明要靠引用分析才能再动）。
## `object` 特化的设计（API 已探明，待实现）

**接口用 shape 指针做内联缓存的键**，因为"属性值指针"只有在形状不变时才有效，而形状只在增删属性时变 —— 固定字段的循环每轮都相等。

**引擎侧（第一段已落地，无调用者）**：
- `JS_WhaleObjNew(ctx, slot)` —— `JS_NewObject` 后存进对象槽（`JS_WhaleObjSlot`，会接管引用）
- `JS_WhaleObjShape(ctx, slot)` —— 返回 `JS_VALUE_GET_OBJ(o)->shape`，不透明键
- `JS_WhaleObjFieldPtr(ctx, slot, atom, &shape)` —— 返回 `&pr->u.value`

**QuickJS 内部用法的准确形态**（照着 `quickjs.c:8774` 的既有写法）：
```c
prs = find_own_property(&pr, p, atom);        /* 7491 处定义，static */
if (!prs) return NULL;
if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL) return NULL;   /* 访问器 */
val = pr->u.value;
```
- 属性数组是 **`p->prop`**（`JSProperty *`，`quickjs.c:1163`），`find_own_property` 会一并给出下标正确的 `JSProperty *`
- 形状是 **`p->shape`**；`JS_PROP_NORMAL` 是 `(0 << 4)`（`quickjs.h:415`）
- `JSProperty` 的第一个成员就是 `JSValue value`，所以 `&pr->u.value` 可以直接给生成的代码解引用

**数值域**：`JS_TAG_INT = 0`、`JS_TAG_FLOAT64 = 8`（**不是 NaN-boxing**），所以内联读是
```c
(fp->tag == 0) ? (double)fp->u.int32 : (fp->tag == 8) ? fp->u.float64 : <deopt>
```
写则是 `fp->tag = 8; fp->u.float64 = v;`。**任何别的 tag 都 deopt**，不要试图内联。

**生成器侧（待做）**：
- `OP_object`（`1,0,1,none`）—— 分配对象槽、发 `if (!JS_WhaleObjNew(ctx, S)) { *deopt = 1; return 0; }`，值就是槽号（同 `OP_get_var` 的 OBJ 分支，**不物化**）
- `OP_define_field`（`5,2,1,atom`）/`OP_put_field`（`5,2,0,atom`）—— 栈上是 `[obj, value]`；**初始化那几个 `define_field` 只调用一次，用 helper 即可**（`JS_DefinePropertyValue`）
- `OP_get_field`（`5,1,1,atom`）/`OP_get_field2`（`5,1,2,atom`）—— **必须在循环里内联**：为每个访问点声明一组 `_fpK`/`_fsK`，先比 shape，再直接读写
- atom 用 `whale_get_u32(pc + pos + 1)` 取，**编译期就到手了**，正好当缓存键
- 别忘了 `whale_load_analyze`/`whale_op_size_of` 那两处（`5` 字节）

**顺序**：先 `object`，通了再照同一套做 `array`（`array_from`/`get_length`，但 `scan(a)` 的数组是参数、形状不可静态确定，更难）。
## `object` 阶段 2 的结果：编进了 JIT，但只到解释器速度

`object.js` 从**完全被拒**（`gen gave up at pos=0 op=11 (object)`，404ms）变成**能编译**（364ms）。**但只快 10%，这是本轮诚实的结论。**

**为什么快不了**：内联缓存确实建立了（每个访问点一对 `_fs`/`_fp`），**但每次属性访问仍要调用一次 `JS_WhaleObjShape(ctx, slot)` 来取当前 shape 做比较** —— 而它要经 `whale_frame_obj_slots(ctx)` 走几次内存解引用，约 5–10ns。2400 万次访问就是 120ms 上下，正好把内联读省下的那点吃回去。**解释器本身也就是这个量级。**

**要真正变快，只有两条路**：
1. **把 shape 检查也内联** —— 需要让生成的 C 看见 `JSObject` 的 `shape` 偏移（或让引擎在固定偏移放一个 shape 字）。**`JSObject` 的头是 `JSGCObjectHeader`，偏移随构建选项变，靠猜太脆** —— 属于需要**刻意设计**的改动（比如在对象槽旁边单独存一个 `void *shape` 副本，由 helper 在解析时写入，生成代码只比较两个指针）。
2. **减少访问次数** —— 把 `o.a += x; o.b += y; o.c += z` 融合成一次调用。那是**语句级融合**，生成器层面很难。

**路线 1 的具体做法（下次可直接做）**：生成代码里每个对象槽再声明一个 `void *_oscK`（shape copy）。`JS_WhaleObjFieldPtr` 解析时顺手写 `_oscK`；比较时生成代码直接比 `_oscK` 与它自己缓存的 `_fs` —— **两个都是生成代码持有的指针，零调用**。只有不等时才调 helper 重新解析。
## `object` 修好了：404ms → **4ms**（node 25ms，反超 6 倍）

上一节的路线 2 生效了，而且是决定性的。做法是**在编译期证明"形状不会变"**，从而把运行时 shape 检查整个去掉：

**判据**（预扫描收集，一次搞定）：
- **函数内没有 `OP_call`/`OP_call0..3`** —— 对象无法被传递出去，外面改不到它；
- **每个 `OP_put_field` 的 atom 都在某个 `OP_define_field` 的 atom 集合里** —— 不会新增属性，属性数组不会重分配；
- 顺带：生成器本来就不支持 `OP_set_field`/`OP_set_array_el`（它们会让函数 `goto out`），所以**动态属性名不可能出现**。

满足这两条时，生成代码里的解析变成**一次性**：
```c
if (!_fs0) { _fs0 = JS_WhaleObjShape(ctx, 0); _fp0 = JS_WhaleObjFieldPtr(ctx, 0, atom, &_fs0); ... }
v0 = (_fp0->tag == 8) ? _fp0->u.float64 : (_fp0->tag == 0) ? (double)_fp0->u.int32 : <冷分支>
```
**之后每轮只剩一次指针判空 + 内联读写**，没有任何函数调用。这正是和"每访问一次调 `JS_WhaleObjShape`"之间的差别 —— 后者只能和解释器打平（364ms），前者反超 node 6 倍（4ms）。

不满足判据时行为不变（每次访问都比 shape），**所以既快又安全**。

途中踩到的四个坑（都值得记）：
1. **新 helper 必须在**生成器 preamble**里也声明** —— 只加 `whale_jit.h` 原型 + `tcc_add_symbol` 不够，缺 preamble 原型会让 TCC 走隐式声明、参数传错。**这是第四次犯同一个错**（前三次是 `fmod`、`JS_WhaleObjProbeFrom`、`JS_WhaleTAU8Ptr`）。
2. `_fp` 必须声明成 **`JSValue *`**，不能是 `void *`（内联读要 `->tag`）。
3. **`v<n>` 在 prologue 已经声明过**，赋值不能写成 `double v<n> = ...`，否则 `redeclaration`。
4. 替换代码块时 `old` 要覆盖**整个**要删的片段 —— 我少覆盖了三行，残留的 `JS_WhaleObjShape(...)` 生成了 `if (!_fs0) { JS_WhaleObjShape(ctx,0)) {`。
## 前端框架方向：优化清单（用户要求列出，待实现）

引擎可能用来跑前端框架，按**对框架的杠杆**排序。前三条是本方向的支柱。

### 1. 小函数内联（最大杠杆）
框架一次渲染会调用上千个小函数（组件的 `render`、`computed` 的求值、`createVNode`）。**每次调用都要建帧、装箱参数、查 memo、可能 deopt**。对"已知的、小的、无副作用的"被调者做内联，收益同时来自三处：省掉调用开销、让常量传播跨过函数边界、让死代码消除看见更多。这是清单里唯一"一条改动通吃所有框架场景"的项。

### 2. Proxy 动态补丁穿透（框架核心）
Vue 3 的响应式就是 `new Proxy(target, { get, set })` —— **每次属性访问都进 JS 陷阱**，那是它的主要开销。可行的做法是识别"单调陷阱"（`get` 只做 `track` + `Reflect.get`，`set` 只做 `trigger` + `Reflect.set`），把它特化成"直接访问 target + 内联 track/trigger"。**风险最高**（要推断用户代码语义），但收益最大，且和已经做好的 `get_field`/`put_field` 内联缓存是同一套机制 —— **target 的属性访问可以复用 `JS_WhaleObjFieldPtr` 那条路**。

### 3. 参数类型特化（按签名分版本）
`scan(a)` 这类"数组/对象作为参数"的通用函数现在整函数进不了 JIT —— 形状不可静态确定。按**调用点签名**分别编译几份（用户早前提过的 C++ template 式做法），每份按"这个参数是 packed array / 这个形状的对象"特化。**它同时解决 `array.js` 的 `scan` 和框架里绝大多数工具函数**（`patch`/`diff`/`normalize`）。

### 4. `Array.prototype` 方法内联
`map`/`filter`/`forEach`/`reduce`/`push`/`slice` 在框架里每帧调上千次。识别"packed array + 内置方法 + 单态回调"后，内联成循环（回调内联见第 1 条）。

### 5. 循环不变量外提（LICM）—— 已有基础设施
`loop_begin`/`loop_end` **已经能把整个循环体缓冲下来**（当初就是为此建的，至今没用）。框架的渲染循环里大量不变式（`list.length`、`props.x`、常量对象），每条外提一次就省一整轮。**这是清单里最容易摘的果子**，因为地基已经在了。

### 6. 多态内联缓存（PIC）
同一个函数被不同形状的对象调用时现在会 deopt。2–4 种形状各留一份缓存（`_fs`/`_fp` 再加一组），能覆盖框架里"同一 patch 函数处理几种 vnode 类型"的常见形态。

### 7. 字符串拼接
模板渲染大量 `a + b + c`。现在字符串上的 `+` 直接 deopt。可行的是识别"拼接链"并走一个 append-only 缓冲（Rust 式 `String`），而不是每步新建字符串。

### 8. `Object.keys` / `for...in`
框架遍历 props/slots。对**我们能证明形状固定**的对象（第 4 步那个判据已经能用）可以直接生成枚举。

### 9. 闭包变量访问
`get_var_ref` 现在只接受"闭包槽就是本函数自己"（自递归）。框架的响应式函数大量捕获外层变量 —— 放开到"闭包槽来自本函数可见的局部"会显著扩大覆盖。

### 10. 循环交换 / 嵌套循环扁平化 + SIMD（用户点名）
改善 CPU 缓存。**但它们的前提是数组访问进 JIT** —— 刚刚打好的 `_ap`/`_an` 缓存 + `get_array_el` 内联正是那个前提。做 2D 遍历（框架里 `matrix`/`table`、虚拟滚动的分块计算）时才划算，**顺序上排在 1–3 之后**：没有参数特化和内联，这些循环根本进不了 JIT。

## `array` 的当前进展（未生效，但地基就绪）

生成器侧已完成：`WHALE_T_ARR` 类型 + `WHALE_ARR_T`/`WHALE_IS_ARR` 宏、`array_from`（只支持空字面量 `[]`）、`get_length`、`get_array_el` 的 packed-array 内联读（元素也是 `JSValue`，同样 `tag == 8/0` 两分支 + 冷 deopt）、`put_array_el`（走 `JS_WhaleArrSetNum`，写后清 `_ai` 标志，因为扩容会移动 backing store）。引擎侧：`JS_WhaleArrPtr`（`fast_array` + `class_id` 检查）、`JS_WhaleArrNew`、`JS_WhaleArrSetNum`（追加走 `add_fast_array_element`，注意它成功接管值、失败时自己释放）。

**但它现在没有收益**，`array.js` 的两个函数仍各自 bail：
- `build(n)` 卡在 `return a` —— 返回数组需要让 `OP_return` 走一条"从对象槽取出引用"的路（`JS_WhaleObjGet` 已有），我加过一版但生成的 C 报 `identifier expected`，**已回退**（宁可干净地不支持，也不要生成有问题的代码）。下次从 dump 里那处 `return JS_WhaleObjGet(ctx, 0);` 入手查。
- `scan(a)` 卡在 `get_length` —— 它的数组是**参数**，需要上面的第 3 步（参数类型特化）。
## 前端框架（React / Vue）方向的评估

用户提议针对 React/Vue 的痛点优化。写了 5 个用例量化后，**结论和直觉不一样**：

| 用例 | node | whalejs | 倍数 | 模拟的框架行为 |
|---|---|---|---|---|
| `immutable_update` | 15 | **693** | **46x** | Redux/React：`{...state, x: v}` 式的状态替换 |
| `objfields` | 20 | **583** | 29x | vnode 记录逐字段更新 |
| `vnode_build` | 16 | **380** | 24x | React `createElement`：每个元素一个新对象 |
| `list_reconcile` | 13 | **157** | 12x | Vue list diff：新旧子节点数组比对 |
| `props_diff` | 2 | 15 | 7.5x | `React.memo` / Vue patch 的浅比较 |

### 关键结论：瓶颈不是 SIMD 也不是缓存，是**内联**

这 5 个用例**全都进不了 JIT**，因为对象都来自**函数调用**（`makeProps`/`createElement`/`setField`），而生成器对"调用返回值"的类型一无所知 —— 它是数值，字段访问无处可施。所以：

- **SIMD 向量化、CPU 缓存友好的循环交换**在这条路上**暂时用不上** —— 它们的**前提是循环已经在 JIT 里**，而这些循环根本不在。
- **`immutable_update` 的 46x 是最典型的**：`setField` 只做"复制 5 个字段 + 换 1 个"，是**完美的小函数内联目标**；内联后 `st` 就是本地对象字面量，形状在编译期可知，**现有的 `_fp` 内联缓存直接生效**（`object` 用例已经是 3ms，反超 8 倍）。
- 所以顺序是 **内联（清单 #1）→ 然后才轮到 SIMD/缓存**。内联是这一族的总闸门。

### 用户提的"objectfield 与 Array 类似"—— 正确用法是**嵌套**

对象字段（`_fp`）和数组元素（`_ap`）现在已经是**同一套机制的两个实例**（编译期缓存 + 内联 tag 读写）。所以"类似"的价值不在把它们合并，而在**让"元素/字段的值本身是对象或数组"也能内联**：

- `nested2d`（`g[r][c]`）正是这个缺口 —— `g[r]` 的类型是数值，第二层访问无从下手。
- Vue 的 vnode 树 diff（`vnode.children[i].props.key`）是同一种形状，而且是 diff 里最热的部分。
- 做法：给"从数组/字段读出来的对象"一个可携带的槽（像 `WHALE_ARR_T` 那样带上槽号），并在读出时做一次类型守卫。**这是 `nested2d` 和 Vue diff 的共同前提。**

### 与清单的关系

- **#1 小函数内联**：这一族的总闸门，现在有 46x/29x/24x 三个数字作依据。
- **#9 闭包**：已完成（`closure` 从 40ms 到 10ms）—— 框架的组件闭包正需要它。
- **#4 `Array.prototype` 内联**：`list_reconcile` 的下一步（`push`/`map`/`filter`）。
- **#2 Proxy 穿透**：Vue 3 响应式的核心，但它是**另一条独立的路**（要识别单调陷阱），和内联无关。
- **#10 SIMD / 循环交换**：**排在最后** —— 需要先有 #1 和嵌套特化，否则循环进不了 JIT。