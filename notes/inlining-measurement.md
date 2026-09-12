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


## 补测：也不是字段解析

把被调函数的内部工作拆成"读 0 / 1 / 3 个字段 + 建 1 个对象"：

| | node | whalejs |
|---|---|---|
| `read3`（读 3 字段） | 50 ms | 352 ms |
| `read0`（读 0 字段） | 14 ms | **331 ms** |
| `read1`（读 1 字段） | 11 ms | 410 ms |

whalejs 的 `read0` 和 `read3` **几乎一样**，而 node 差 3.5 倍。所以字段解析
（per-call `find_own_property`）**不是**这 300ns 的来源——把它优化掉也不会动这个数。
（顺带说明 `read1` 比 `read3` 还慢是噪声：这条测量在 `--repeat` 上跑一次，抖动量级就是几十 ms。）

结合前面：纯调用 ~55ns，字段解析不是大头，而"调用 + 建一个对象"就是 331ms/100万 = 331ns，
同样的工作在展开版里是 10ns。**差异集中在"被调者里的对象字面量"这一个组合上**，
具体机制还没确定（候选：跨帧的分配/释放路径、或 `whale_needs_obj` 那 12 个槽的
初始化与释放每次都重来）。**在查清它之前，做跨函数内联是在赌一个自己还不理解的开销。**

## 路径是存在的，但不在这一轮的量级

用户提的"在 QuickJS 侧展开"是对的，而且比想象的近：`JSWhaleFuncInfo` 里**已经有**
`cpool`（`const JSValue *`），而函数字面量就住在 `cpool` 里——`OP_fclosure <idx>` 取的就是它。
所以生成器不需要新的编译器支持就能看到被调者的字节码：

1. 预扫描识别 `OP_fclosure M` + `OP_put_locN` → `local_fn[N] = M`
2. `OP_get_locN` 时若已知，标成"这个槽里的函数是 cpool[M]"（新类型，编码同 `WHALE_T_VAL`）
3. `OP_call` 时据此展开

`whale_jit.c` 现在还不认识 `JSFunctionBytecode`（它刻意"knows nothing about quickjs internals"），
所以要加一个引擎侧入口，比如 `JS_WhaleCpoolFuncInfo(ctx, cpool, idx, &out)`，
把那个函数的 `byte_code`/`var_count`/`stack_size`/`arg_count` 填进一个 `JSWhaleFuncInfo`。

**但第 3 步是真正的字节码展开**：callee 的参数、局部变量、栈槽都要映射到 caller 的槽位，
`return` 要跳回调用点，跳转偏移要重定位。这是几百行新代码，外加一整套新的正确性测试
（尤其"被内联的函数自己也有对象字面量"这种情况，两个函数的对象槽必须不重叠）。

**结论：值得做，但应该是一轮自己的事，而不是这一轮的尾巴。**


## 定位到了：那 ~200ms 在**对象返回**路径上

把被调者拆成四种，都是 100 万次调用：

| 被调者 | node | whalejs |
|---|---|---|
| `f1(o,v){return v+1;}` 纯数值 | 13 ms | **83–95 ms** |
| `f2(o,v){var q={a:v,b:v}; return v+1;}` 建对象、数值返回 | 11 ms | **93–111 ms** |
| `f3(o,v){return {a:v,b:v};}` 建对象、**对象返回** | 11 ms | **282–310 ms** |
| `f4(o,v){var q={a:v,b:v}; return q.a;}` 建对象、字段读、数值返回 | 7 ms | **84–89 ms** |

**`f3` 和 `f2` 只在"返回的是什么"上不同，差 ~190ms。** 建对象本身不是问题（f2/f4 都是 ~90），
字段读也不是（f4 甚至最快）。**成本集中在 `ret_obj` 这条返回路径上。**

## 但两次有界优化都没打中它

1. **不整体清零 `_ws`**（约 1KB → 只清 40 个 int）：**变慢**（`fib` 9→49ms）。
   编译器对 `memset` 生成的清零比手写循环高效得多。已回退。
2. **对象返回用 take 而不是 get**（槽把引用直接交出，省一次 `js_dup` + 一次 `JS_FreeValue`）：
   **无效果**（`f3` 282→310，在噪声内）。已回退。

所以这 ~190ms 不在 dup/free 往返上，也不在 `_ws` 初始化上。它更可能在
`JS_CallInternal` 收尾时对"帧里躺着一个对象返回值"的处理上（对象槽的释放 +
`local_buf..stack_buf` 的清理 + memo 路径），或者与 GC 的交互——**要再往下走需要时间剖析，
而这台机器上没有 profiler。**

## 但结论变了：内联不必等这个查清

内联**完全绕过**这条路径——没有调用、没有返回、没有 `ret_obj`，被调者的对象字面量变成
调用点的一段代码（`inlineOnly` 已经证明那是 10ms）。所以第 1 条（查清 ~190ms）不再是
第 2 条（内联）的前提：**内联的收益是确定的，无论那 190ms 的机制是什么。**

如果将来还想查它，入口在这里：`JS_CallInternal` 里 JIT 成功返回后的那一段
（`whale_needs_obj` 的对象槽释放循环 + `local_buf..stack_buf` 清理 + memo 插入）。
