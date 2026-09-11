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

## 验证

1. **先不用 JIT**：写一个 `register double` 的小 C 文件，走 `whale_tcc_compile` 的 C 源路径，确认语义正确（这是唯一不依赖 `whale_jit.c` 的隔离验证）。
2. 再让 `whale_jit.c` 对循环变量发 `TOK_REGISTER`，跑 `node perftest/main.js --engines=node,whalejs`，`check` 必须跨引擎一致。
3. 性能看 `arith`/`locals`/`loop_sum`/`bitops`/`minmax` 这类纯数值循环（现状 1.7×–3.4×）。notes `<19>` 预估寄存器保留 +20~50%。
4. 回归重点：带 `call` 的函数（`fib`）、`sieve`（走对象槽那条路）。

## 另一条已被否掉的路

在 TCC 单遍流式发码的现状下做**循环级**优化（把归纳变量固定在寄存器）**必须能看到控制流**。notes `<19>` 的结论是"必须 IR 直出通了才动得了"—— 但 `<34>` 之后走的是 **token 通路**（生成器发 token 给 TCC parser），IR 直出并没做。所以循环优化仍然只能在 `whale_jit.c` 侧做（它有完整的循环体），**而"固定寄存器"是 TCC 侧唯一缺的那块**，这正是本方案要补的。
