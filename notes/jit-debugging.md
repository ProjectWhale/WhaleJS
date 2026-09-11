# JIT 调试与用法速查

## 环境变量

| 变量 | 在哪里 | 作用 |
|---|---|---|
| `WHALE_JIT_DUMP` | `whale_jit.c`(804/2071/2141/2202/2241)、`quickjs.c`(3211/3517/18862)、`tccpe.c`(792/1225)、`x86_64-link.c`(266) | **主诊断开关**。转储生成源码；报告生成器放弃点 `whale jit: gen gave up at pos=<n> op=<n> (<name>)`；报告 deopt `whale jit: deopt, back to the interpreter`；打印 PE import 重建过程 |
| `WHALE_IR_DEBUG` | `whale_tcc_iface.c`(151–261, 564) | 打印 ISA 位图，并转储 **token 流**（`JS_WhaleTokDump`）。**只在编译成功路径调用** — 没输出 = 没编成 |
| `WHALE_ISA` | `libtcc.c:763` | 覆盖 CPUID 探测出的 ISA 位 |
| `WHALE_MEM_TRACE` | `whale_jit.c`(734/755/782) | 延迟局部物化追踪：`[mat] L<d> off=.. len=..`、`[inl] L<d> live=..`、`[set] L<d> off=.. len=..` |
| `WHALE_OBJ_TEST` | `whale_jit.c:2035` | 对象槽 GC 存活测试（强制 `JS_RunGC()`） |
| `WHALE_NO_RESET` | `tccrun.c:217` | TCC 运行期不重置 |

## 最容易搞混的一对

- **`WHALE_JIT_DUMP`** 回答 **“为什么没 JIT / 编译到哪一步放弃了”**
- **`WHALE_IR_DEBUG`** 回答 **“JIT 成功时我到底生成了什么”**

因为 `JS_WhaleTokDump()` 挂在成功路径上，**`WHALE_IR_DEBUG=1` 输出为空，不是“没有内容”，而是“这个函数压根没编成”**。查“没生效”必须用 `WHALE_JIT_DUMP`。

（踩过：`bitops` 掉回解释器，我用 `WHALE_IR_DEBUG` 看 dump 得到空文件，结论误判成“编译成功但输出丢失”，白花了两轮构建去找一个根本不存在的输出问题。真实原因是一个 `goto out`，`WHALE_JIT_DUMP` 一行就说明白了。）

## “JIT 没生效”的标准三步

```powershell
$env:PATH = "<repo>\build\mingw\x86_64\release;$env:PATH"
$env:WHALE_JIT_DUMP = "1"
.\build\mingw\x86_64\release\whalejs.exe perftest\cases\<case>.js
```

1. **看有没有 `gen gave up`** — 有就是生成器在某个 opcode 上放弃，`pos` + `op` + 名字直接定位（`whale_op_name(int)`，定义在 `whale_jit.c:403`）。整个函数会静默退回解释器。
2. **看有没有 `deopt`** — 编译成功但运行时守卫失败，也是退回解释器。
3. **都没有** — JIT 在跑，问题在生成代码的质量（回到 `WHALE_MEM_TRACE` 和 `WHALE_JIT_DUMP` 的源码 dump）。

## 测量

- `node perftest/main.js --engines=node,whalejs` —— 全量，`check` 必须跨引擎一致
- `--repeat=N` 已存在，取 **N 次里最小的**（不是中位数）。`fib` 这类很短的 case 单次波动可达 10–17ms，**必须多轮**才有意义
- **跑基准前确认没有构建在跑**：MinGW 链接会吃满所有核，node 那侧的数字会虚高（实测 `arith 137→64ms`、`array 208→20ms`）

## 生成器（`src/whale_jit.c`）里必须知道的三件事

### 1. 三层缓冲

```
token sink (gw_tok/gw_num)  ──fold_commit──▶  输出流 (go_* 或 whale_loops[d].*)
        ▲
        └── fold_inline(k)：把栈槽 k 的值*追加到 sink 当前末尾*（即嵌进正在编的表达式）
```

- `fold_mark()` / `fold_commit(mark)` 划出 sink 里的一段并拷到输出
- `whale_vexpr[k] = {off, len}` 是**栈槽 k 的值 = sink 里的一段**
- `fold_materialize(k)` 是**唯一**置 `whale_vmat[k]=1` 的地方，它同时把 `v<n> = ...;` 这条语句 commit 到输出
- `fold_inline(k)` 在 `whale_vmat[k]=1` 时只发 `v<n>`，否则内联整段表达式
- **`v<n>` 不是 SSA 变量**：每次物化取新名字（`whale_vname[k]`，计数器 `whale_vserial`），因为 pending 表达式把名字作为 token 记了下来，改写同名变量会静默改掉它读到的值

### 2. 语句数才是性能

`bitops` 循环体 24 条语句，其中 **10 条来自 `fold_set` 看到表达式超过 `WHALE_EXPR_MAX`(24 token) 时立即物化**。所以**“缩短表达式”比“删语句”更根本** —— 长表达式会自己长出语句。TinyCC 不做 CSE，每个操作数重复展开几次就是几倍的指令。

### 3. 类型合并会拒绝整个函数

跳转目标的状态合并（`whale_jit.c` ~1030）把**任何类型差异**当作冲突 → `goto out` → 整个函数不编译。加新类型时必须让它和语义等价的类型**向下合并**，否则像 `WHALE_T_I32` vs `WHALE_T_NUM` 这种“只是更精确”的差别会让函数整个掉出去。

## 加一个 opcode 的检查清单

1. `switch (op)` 里加 case，用 `whale_type_is_num(t)` 判断数值类型（**不要写 `t == WHALE_T_NUM`**，会漏掉 `BOOL`/`I32`）
2. 生成的值要 `fold_set(k, fold_mark())`，并设 `cur[var_count + k]` 的类型
3. 跨基本块的值必须先 `fold_materialize`；跳转前 `local_flush_all`
4. 别忘了 `whale_load_analyze`（peephole 用）和 `whale_op_size_of`（引擎侧）
5. 验证：`WHALE_JIT_DUMP=1` 应无 `gen gave up`，且 `check` 与 node 一致
