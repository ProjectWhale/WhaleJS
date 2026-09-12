# 内联（A4/D1）：先量了一次，结论和预期不一样

## 测的是什么

`immutable_update` 的形状在 whalejs 上是 568ms、node 13ms。原因要么是**调用开销**，
要么是**被调函数内部的开销**。用同一段逻辑的两个写法把它分开：

```js
function sf(o, v) { return { a: o.a, b: o.b, e: v }; }
function callVer(n)   { var st = {a:0,b:0,e:0}; for (i) { st = sf(st, i % 11); st.a = st.a + st.e; } return st.a; }
function inlineVer(n) { var st = {a:0,b:0,e:0}; for (i) { var v = i % 11; st = { a: st.a, b: st.b, e: v }; st.a = st.a + st.e; } return st.a; }
```

100 万次迭代（20 × 50000）：

| | node | whalejs |
|---|---|---|
| `callVer` | 16 ms | **372 ms** |
| `inlineVer` | 13 ms | **10 ms** |
| `callOnly`（循环里只有调用） | 10 ms | **359 ms** |
| `inlineOnly`（循环里只有字面量重建） | 9 ms | **10 ms** |

**内联版在 whalejs 上是 10ms —— 和 node 持平。** 同样的工作，只要不跨调用，就是快的。

## 再把"调用"本身拆开

| | node | whalejs | 每次调用 |
|---|---|---|---|
| `call0`（空函数，0 参数） | 2 ms | 37 ms | ~37 ns |
| `call2`（空函数，2 参数） | 2 ms | 59 ms | ~59 ns |
| `loopOnly`（空循环） | 2 ms | 0 ms | — |

**纯调用开销 ≈ 40–60ns，不是 300ns。** 所以 `callOnly` 的 359ms 里，
约 60ms 是调用、约 300ms 是**被调函数内部的工作**（读 3 个字段、建 1 个对象）。

## 结论：300ms 花在"每次调用重新解析字段"

`sf` 的 `o.a` 走 `fold_obj_field_load`，它的缓存对 `(_ws.fs[fc]` 形状、`_ws.fp[fc]` 地址 `)`
是**每个调用**的状态（`_ws` 是 core 的局部变量）。

- 在 `step` 的循环里：一次解析，用 50000 次
- 在 `sf` 里：**每到一次就重新解析一次**，因为它每次收到的是**不同的对象**，
  缓存的地址本来就必须失效

所以 `sf` 的每次调用 = 建帧 + 3 次 `find_own_property` + 建对象 + 拆装箱。
这正是解释器的成本结构，也解释了 359ms ≈ 解释器速度。

## 为什么不能直接内联

字节码内联要求**编译期知道被调者的字节码**。而 `sf` 在 `step` 里是
`get_var_ref0`（闭包引用）/`get_var`（全局）—— 它的值来自**运行时**的
`var_refs`/全局对象，`cpool` 里没有 `JSFunctionBytecode`。这是 QuickJS 闭包模型
决定的，不是我实现得不够。

内联要做，只能走"运行时特化"：在调用点缓存"上次的 callee 是哪个函数 + 它的入口"，
并**验证**它没变。那是 D3（按调用点签名分版本）家族的事，不是 A4 的字节码内联。

## 真正能拿到这 300ms 的路径

把字段缓存从"值地址"升级为**"形状 → 属性下标"**，并且让它**跨调用**有效：

- 形状是稳定的（同一构造序列共享 shape），所以 `shape → index` 可以常驻
  （挂在 `JSFunctionBytecode` 上，像 `whale_memo` 那样），而不是每次调用重学
- 有了 index，取地址只需按**当前**对象算：`p->prop[idx]`，不再做 atom 比较

代价是取地址仍要一次 helper（除非把 `JSObject.prop` 和 `JSProperty.u.value` 的偏移
暴露成编译期常量让生成代码自己算 —— `JS_WhalePropOffset()` 之类的三个常量即可，
但那样就把对象布局钉进了生成代码，和 notes 里"别硬编码 JSObject 布局"的告诫冲突，
需要想清楚再动）。

**这一步（D6 的"IC 提到函数级"）是 `immutable_update` 这类形状的真正出口，
而不是内联。** 内联在"被调者编译期不可见"时没有立足点。
