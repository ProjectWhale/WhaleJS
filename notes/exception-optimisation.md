# 异常处理的性能优化方向

Status: **待办**。try/catch 已能原生编译且语义正确（`fcf14bf`），但**慢于不编译**。

## 现状数据

一个 `try { if (x<0) throw new Error(m); return x*2; } catch(e) { return -1; }`
的函数，在循环里调用 1e6 次：

    whalejs 编译后   225ms
    whalejs 解释器   156ms     ← 编译后反而更慢
    node              5ms

**编译后比解释器还慢**，这说明生成的路径做了解释器不做的额外工作。

## 每次调用付出的代价（来自 `WHALE_JIT_SRC` dump）

    JS_WhaleObjSlotUndef ( ctx , 0 ) ;                      ← catch 槽初始化
    ( ! JS_WhaleGetGlobalObj ( ctx , 160 , 1 ) ) { ... }     ← 查全局 Error
    ( ! JS_WhaleObjSlotAtom ( ctx , 683 , 2 ) ) { ... }      ← "neg" 字符串常量
    ( ! JS_WhaleConstructValue ( ctx , 1 , 1 , 20 , 3 ) ) { ... }  ← new Error

四次 helper 调用，**全都在正常路径上**——也就是 1e6 次调用里，99.99% 的调用
`x >= 0`，`throw` 一次都没发生，但上面四次都做了。

## 优化方向（按收益/风险排序）

### 1. catch 槽的初始化推迟到 handler 入口（收益最大、最干净）

现在 `OP_catch` 生成 `JS_WhaleObjSlotUndef(ctx, os)`，把 undefined 停进槽里，
因为 catch 块会从那个槽读异常值。但**只有真正抛异常时**才会进 catch 块——
正常路径上这个槽根本没人读。

- 做法：`OP_catch` **不**生成槽初始化；在 `catch` 目标（handler 入口）处生成
  `JS_WhaleObjSlotUninit` 之类的标记，或者在**每条** `goto L<handler>` 之前
  （`OP_throw`）确保槽已经设好异常值。
- 风险：handler 也可能由**其它**跳转到达（嵌套 try、`continue` 等），需要确认
  所有入口都覆盖。`WHALE_OP_DBG` 的 `[targets]` 能列出谁指向 handler。

### 2. 全局查找与字符串常量只在抛点求值

`JS_WhaleGetGlobalObj(ctx, Error)` 和 `JS_WhaleObjSlotAtom(ctx, "neg")` 是
`throw new Error("neg")` 的**准备**工作，但它们在**正常路径**上执行——因为
字节码是 `catch; if(...) goto L25; get_var Error; dup; push_atom_value; call_constructor; throw; L25: ...`。

- 这其实是**字节码顺序**决定的，不是生成器的错：准备代码本就在 `if` 之后。
- 但生成器可以把 `get_var`/`push_atom_value` 的槽**懒**初始化：整个
  `try` 体内第一次需要时才求值。收益取决于 try 体的形状，**不一定**值得。
- 更根本的是：`new Error(...)` 本身调用 `JS_WhaleConstructValue`，那是 `new`
  的真实成本，去掉不了。除非将来对**已知内建构造器**做特化。

### 3. `JS_WhaleObjSlotUndef` 不必是函数调用

它只做 `JS_WhaleObjSlot(ctx, k, JS_UNDEFINED)`。`JS_UNDEFINED` 的表示是编译期
常量（`WHALE_UNDEF_DBG` 能打印出来，注释里记着 tag 值和字宽），所以生成器可以
直接写 `_ws` 槽的那两个字段，省一次调用。方向 1 做了之后这条自然消失。

### 4. 和 for 慢 4 倍同源：原生代码与 VM 的交接

`objfields`（4 次字段访问/迭代 + 2 次取模）与 `bitops` 都还没到原生 C 的水平，
而 `vnode_build` 编译后仍贴着解释器时间。这些和上面 1-3 是**同一类**问题：
每次跨 helper/VM 边界的成本，以及循环体里不必要的重复工作。

## 必须做的对照实验（欠账）

写一段和 `perftest` 各用例近似的 C，用 **-O0 和 -O1** 编译自己跑，得到**原生
上限**。没有这个基线，就无法判断某个用例是"已经到原生上限"还是"生成代码有
额外开销 / 生成有误"。这是判断一切优化是否有意义的前提，应该**尽早**做。

建议对 `objfields`、`bitops`、`sieve`、`nested` 各写一份：
- 手写等价 C（用 `double`、无装箱）
- `gcc -O0` / `gcc -O1` / `gcc -O2` 各跑一遍
- 和 whalejs 的 JIT 时间、原版 qjs、node 四者并排

## 相关提交

- `fcf14bf` — try/catch 能工作（三个 bug：core 里 return JSValue、catch 槽 fold
  状态、`nip_catch` 必须**搬**值而不是只 `sp--`）
- `a683227` — `new F(...)` 降低 + `whale_is_call_op`/`feeds_call_as_callee` 修复
