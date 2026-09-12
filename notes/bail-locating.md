# 定位"生成器为什么放弃这个函数"的方法

## 问题：dump 报的 pos 常常不是放弃的原因

`whale_jit_gen` 有 61 个 `goto out`，全都跳到同一个 `out:`。已有的诊断
（`WHALE_JIT_DUMP`）打印"在哪条指令上放弃"，但这经常**不是**放弃的原因所在。

排查 `immutable_update` 的循环体时，dump 说：

```
whale jit: gen gave up at pos=44 op=203 (get_loc0)
```

于是所有注意力都跑到 `get_loc0` 上。真正放弃的是**跳转目标的状态合并**
（`if (differ) goto out;`），而它发生在处理任意指令之前、恢复快照的那一小段里 ——
`get_loc0` 只是恰好是那一轮的跳转目标。

## 方法：给每个 bail 打上行号

一次替换 + 一处打印，就把"哪个 goto"变成事实：

```c
/* 61 处 goto out; 全部换成 */
{ whale_out_line = __LINE__; goto out; }

/* out: 处的诊断加一个字段 */
fprintf(stderr, "gen gave up at pos=%d op=%d (%s) bail=whale_jit.c:%d\n",
        pos, bad, whale_op_name(bad), whale_out_line);
```

**坑**：`goto` 不是表达式，**不能用逗号表达式** ——
`whale_out_line = __LINE__, goto out;` 会报 `expected expression before 'goto'`。
必须是 `{ …; goto out; }` 这种块语句。

于是同一个 dump 变成：

```
whale jit: gen gave up at pos=44 op=203 (get_loc0) bail=whale_jit.c:1499
```

1499 就是 `if (differ) goto out;` —— 一行定案。

## 它立刻换来了什么

顺着行号加打印，得到：

```
[merge] pos=44 local=0 was=40 now=72
```

`40 = WHALE_VAL_T(1)`，`72 = WHALE_VAL_T(3)` —— **两个都是 VAL，但槽号不同**。

而槽统一的逻辑本身是对的（`[put_loc] pos=19 loc_slot=1 src=0 newt=40`），
关键的是：**循环体里的那次赋值根本没走 `put_loc`**（只打印出一次 `put_loc`，
那是循环外的初始化）。也就是说 `cur[0]` 是被**别处**设成 `VAL(3)` 的，
3 是 `whale_objs++` 的第四个值。

**这个问题还没解决，但它的形状变了**：从"类型合并规则不对"变成
"某个尚未找到的地方在写 `cur[0]`" —— 这比原来的猜测具体得多，也是下一步的入口。

## 教训

`gen gave up` 报的是**当前指令**，不是**放弃的原因**。当 dump 的 `op`
看起来完全正常（`get_loc0` 不该失败）时，原因在别处。给每个 bail 打上行号
是最便宜的一步：一次替换，之后永久可用。
