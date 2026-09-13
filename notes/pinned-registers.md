# Pinned registers in TinyCC: why `register` currently loses

## The mechanism that exists

`register` on a local is taken seriously (tccgen.c:4257). The local gets a
register that `get_reg()` will not hand out (`whale_pinned_regs` mask), and reads
(1396), operands (2268) and stores (3318) all know about it, so the value stays in
the register across statements instead of round-tripping through the frame.

Allocation is in `whale_pin_alloc` (975):

```c
for (r = NB_REGS - 1; r >= 18; r--)
    if ((reg_classes[r] & RC_FLOAT) != 0)
        if ((whale_pinned_regs & (1u << r)) == 0) { ... return r; }
return -1;
```

## Two separate problems, measured

**1. Three pins is already one too many.**

`NB_REGS - 1` is 21, so the range 18..21 offers exactly four float registers. With
the generator's `whale_nloops <= 1` limit lifted so that nested loops get pins:

| lower bound | pins available | `f(1000); f(2000000)` | `minmax.js` |
| --- | --- | --- | --- |
| 20 | 2 | 6000046 (correct) | 118 ms |
| 19 | 3 | **hang** | **hang** |
| 18 | 4 | **hang** | **hang** |

TinyCC has about 14 xmm registers and needs some of them for the intermediate
results of its own expression evaluation. `get_reg()` scans for a free one, and a
pinned register is invisible to it. When enough are pinned, `get_reg()` returns
-1 - and its callers assume it cannot fail, so the generated code is wrong from
there on. In this case a loop counter stops advancing and the loop never ends.

The existing lower bound of 18 is a **workaround for exactly this**, not a fix. The
comment says so: "pinning away every float register leaves get_reg() with nothing
to spill and returns -1, which is how minmax (five locals, two nested loops) ended
up looping for ever".

**2. Even where it works, pinning is slower.**

At the safe setting (2 pins, lower bound 20), with nested loops now eligible:

```
bitops     165 ms  ->  337 ms
```

Pinning made the hottest loop **twice as slow**. Two float registers held
permanently means two fewer for TinyCC's own expression evaluation, so more
spilling - and the thing being saved (a frame slot access) is cheap to begin with.
An earlier measurement found the same: moving live values from `_ws.v[]` into C
locals changed nothing.

## What this means for the backend work

The reason `register` pays nothing is not the idea, it is that **`get_reg()` has
no way to reclaim a pinned register under pressure**. Pinning is currently an
absolute claim on a register that the register allocator cannot veto.

So the concrete first task in the backend is:

> let `get_reg()` spill a pinned register when it runs out - write the value back
> to its frame slot and update the offset -> register map, so that the subsequent
> reads know to read the slot again until the next write.

Without that, any pin count high enough to matter breaks code generation, and any
pin count low enough to be safe is a net loss. With it, the pin map becomes a
hint the allocator can weigh rather than a claim it must honour - which is the
point at which a "no stack" calling convention could start to pay off.

The current state is left as it was: lower bound 18, `whale_nloops <= 1`, 24 cases
matching node. The scope fix from the previous commit (one pin map per JIT
compilation, not per C function) is kept - it is correct and verified independently
of all of the above.
