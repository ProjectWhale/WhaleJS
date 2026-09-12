# 值域扩展：闭包调用目标、任意值、双版本编译

## 起点：三个缺口其实是同一个形状

三个最大的 front-end 缺口（`objfields` 621ms、`immutable_update` 693ms、
`vnode_build` 420ms）看起来各不相同，实际都是：

```js
function make() { return { tag: 0, key: 0 }; }
function run(n) { var o = make(); for (...) { o.tag = i; } return ...; }
```

对象**来自一次调用**，所以它的形状只能在运行时知道。

## 障碍一：闭包调用目标有两种读法，而字节码不区分

`make()` 编译成 `get_var_ref0` 后跟 `call0` —— 与自递归（`fib`）**完全相同的形状**。
生成器两者都读作"被调者就是这个函数"，因为那正是 `fib` 不需要装箱的原因。

对"调用自己捕获到的辅助函数"的函数，这个猜测是错的，入口检查
（`JS_WhaleVarRefIsSelf`）失败，整个函数退回解释器。

**只有运行时知道哪种读法对，所以两种都编译：**

- 生成器不再猜，从引擎拿 `whale_assume_self`
- `whale_on_call` 先编 self 读法，再编 closure 读法（当生成器报告存在这样的槽，
  或当第一种压根没编过时）。**两种都可能编不过，且这是预期的**：调用捕获辅助函数的
  函数没有可用的 self 读法；递归函数没有能对结果做算术的 closure 读法
- 两种都编过时，入口检查失败设 `*deopt == 3`。这个码与其它所有 deopt 都不同 ——
  它是**唯一**在函数体任何部分执行之前发生的 —— 所以 `JS_CallInternal` 可以**只凭这个码**
  切到另一个编译版本。宿主调用绝不会被执行两次
- memo 只按 self 读法建立（那是生成器证明"只依赖参数"的那个）；切到 closure 读法时关掉

## 障碍二：值域里没有"任意值"

新增 `WHALE_T_VAL` —— 停在对象槽里的任意 JSValue，与 `WHALE_T_OBJ`/`WHALE_T_ARR`
同一套槽编码，用 `WHALE_IS_SLOT`/`WHALE_SLOT` 统一取槽。

- 调用可以有**活结果**（`JS_WhaleCallValue` 多一个 `ret_slot`）。既没被丢弃、又不可用的
  结果仍然拒绝 —— 因为之后向它要数值会 deopt，而重跑帧会把调用**再执行一次**
- `WHALE_T_VAL` 能在 `get_loc`/`put_loc` 里传播，并作为 `get_field`/`put_field`/`get_field2`
  的**对象**（`o.tag = i` 里 `o` 来自调用时要的正是这个）。这样的对象**始终保留形状检查** ——
  `stable` 是就本地字面量证明的，而它不是本地字面量（`whale_shape_stable && WHALE_IS_OBJ(ta)`）
- 调用的参数/结果暂存在对象槽顶部（`WHALE_CALL_BASE`，槽数 8→12），对象字面量/数组被限制在
  基址以下，两个区域永不重叠

## 结果

`objfields` 621 → **173ms**，`fib` 保持 ~8ms，24 个 perftest case **0 mismatch**。

## 剩下两个用例卡在哪（下一步）

- **`immutable_update`**：`function setField(o, v) { return { a: o.a, b: o.b, ... }; }` ——
  **参数作为对象**（`o.a` 的 `o` 是参数）。参数目前只有"数值"一种读法，需要按用途决定读法，
  即**参数类型特化**。它的调用点 `setField(st, i % 11)` 已经在支持范围内（参数可以是对象）。
- **`vnode_build`**：内层对象有字符串字段（`cls: "c" + (i % 3)`）和 `key: null`。
  需要非数值字面量（`push_atom_value`、`null`）与字符串拼接。

两者都属于路线图里排在后面的项（D3 参数特化、D7 字符串拼接），不是这一步能顺手带过的。

## 工具教训（重复踩了两次）

`xmake` 的增量判断在这台机器上不可靠：改了 `whale_jit.c` 后 `xmake -y` 只用 3 秒
"cache compiling"，二进制没更新。**判断构建是否真的发生，看二进制里有没有新符号**
（`[System.Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes(exe)).Contains("新符号名")`），
需要时用 `xmake -r -y`。

另外：`quickjs.c` 里有几处**混合行尾**（少数 CRLF 夹在大量 LF 中）。用
`$t.Contains($old)` 做补丁时，跨行锚点会因此失配且报错位置具有误导性。
补丁脚本应先 `$t.Replace($CRLF, $LF)` 归一化，再做替换。
