# Optimisation directions, ranked by evidence

## 1. Where the real wins are: statement and instruction count

The only improvement measured between two *correct* builds was in this area:
`ToInt32` simplified to one instruction and `%` strength-reduced to a mask moved
`(a + i*3) % 65536` from 113ms to 73ms (node: 11). That is a 35% gain from making
the generated code shorter, not from moving values around.

Candidates, none of them started:

- **LICM for the object/array guards.** `fold_obj_field_load` emits a shape check
  per access; inside a loop whose object does not change, that check is loop
  invariant. The `loop_begin`/`loop_end` machinery in the generator buffers the loop
  body already and has never been used for anything.
- **Constant-fold `_ws` index arithmetic.** Every value slot access is
  `_ws.v[<n>]` with a compile-time `n`; the address is `rbp + const`, but whether the
  generator emits it that way (versus a computed offset) has not been checked.
- **Reduce helper-call density** (see below).

## 2. Reduce helper-call density

Not yet quantified, but it is the reason pinning regressed on some cases before the
xmm6/7 change: `JS_WhaleObjNew`, `JS_WhaleObjDefineNum`, `JS_WhaleArrPtr`,
`whale_jit_box_num`, `JS_WhaleCallValue` and friends are **function calls**, and a
call has costs beyond itself:

- it forces every live value in a register to be saved (spill) and reloaded
- it blocks any optimisation that wants to keep values in registers across it

Where a helper is trivial (a bounds check, a tag check, an index computation) it may
be cheaper to inline than to call. Worth measuring: count helper calls per case
statically from the generated C, compare against the loop body statement count.

## 3. Register pinning — done, marginal

Now uses **xmm6/xmm7** (indices 22/23), which the register table marks as callee-saved
on Windows. A value parked there survives calls with no instruction from us, which
removed the cost that made pinning net-negative on the caller-saved xmm2-xmm5.

Alternating A/B measurement (same machine, both builds interleaved, minimum of 5):

    nested   94 -> 85    -10%
    array    90 -> 86     -4%
    minmax  115 -> 113    -2%
    bitops  159 -> 157    -1%
    the rest      within noise

Two registers is the ceiling (there are only two callee-saved float registers in the
Windows ABI), so this is a marginal win at best. It pays nothing on System V, where
the code is compiled out.

## 4. SIMD

Blocked on 1. Needs a loop-level vectorisation pass and a float pool that can hold
more than one value per register; the allocator and the pin machinery both assume
scalar. Deferred.

## Measurement discipline, learned the hard way

Single-run and few-sample timings on this machine vary by 40%+. Three separate wrong
conclusions came from that:

- "25-30% from pinning" - the number that motivated this whole line of work
- "pinning is net negative" - seen on a busy machine, and again on a quiet one when
  comparing two builds measured at different times
- "bitops regresses 8%" - time drift between the two measurement points

Use `temp/measure.ps1` (N runs, report the minimum) **and alternate the builds**:
measure A, rebuild B, measure B, rebuild A, measure A again. If the two A numbers do
not agree, the machine is drifting and the comparison is not evidence.
