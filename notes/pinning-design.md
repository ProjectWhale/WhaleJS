# Pinning, final design: make it dynamic instead of absolute

## The exact hole

`gfunc_call` (x86_64-gen.c:1030) emits the call:

```c
    save_regs(0);          /* 1020 — spills everything still live */
    ...
    gcall_or_jmp(0);       /* 1030 — the call */
```

`save_regs` → `save_reg_upstack`, and **our patch makes that function return
immediately for a pinned register**:

```c
    if (whale_pinned_regs & (1u << r))
        return;            /* "nothing on the stack can force its value out" */
```

So at a call the pinned register is neither saved nor invalidated, while the call is
free to destroy it (every xmm is caller-saved on x86-64). Afterwards `gv()`'s pin
shortcut still hands out that register, now holding whatever the callee left.

That is the whole bug. Everything else measured (frame size, callee-saved GPRs, stack
cleanup, get_reg collisions) was checked and ruled out.

## The fix: a validity bit per pinned local

The current model says "a pinned local's value is in its register, always". That is
an absolute claim, and calls are exactly what breaks an absolute claim. Make it a
claim that can be temporarily suspended:

| event | action |
| --- | --- |
| write to a pinned local | value goes to the register; register **valid**; frame slot stale |
| **call site** | if valid: `movsd %xmmN, slot` — store to the frame slot and mark **invalid** |
| read a pinned local | if valid: return the register. if invalid: load from the slot into the register, mark valid again |

The frame slot then does two jobs it already does for normal locals: it is the
authoritative copy whenever the register cannot be trusted.

**Where to implement**: `save_reg_upstack` is already called at exactly the right
moments (it exists to make a register free before a call). Instead of returning early
for a pinned register, it should spill it and clear the valid bit. The `gv()` shortcut
then needs the valid bit so it knows whether to re-load.

This costs 1 store + 1 load **per call**, not per statement. A loop with six statements
and one call still saves six accesses to pay two.

## Second problem: get_reg exhaustion

Independent of the above: pinning three registers makes `get_reg()` fail (it returns
-1, which its callers treat as impossible). It has no way to reclaim a pinned
register. The fix is for `get_reg` to **evict** a pinned register when it runs out -
write it back to its slot, mark it invalid, return it. That degrades that local to an
ordinary one for the rest of the function, which is correct and always safe.

Note `save_reg_upstack`'s early return is what makes the exhaustion fatal: even when
`get_reg`'s spill loop picks a pinned register, the spill does nothing and a register
still holding live data is handed out. **These two are the same bug.**

## Third: the generator's choice of variables

The pin gate is `!whale_has_call && whale_nloops <= 1` - "no calls and at most one
back edge". Both halves are workarounds for the correctness hole above, and both
should go away once the validity bit exists.

What should replace them is a proper answer to "which locals are worth a register":

- assign_cnt / induction[] already exist (pre-scan, per local: reads, writes, and
  whether it is a loop counter)
- the earlier measurement says the right two locals buy 25-30%, the wrong two cost
  118% - so this choice dominates the outcome
- the ABI-relevant part: a local that is live **across a call** costs 2 moves per
  call, so locals whose live range avoids calls are better candidates. That is
  computable from the bytecode the generator already scans.

## What was verified, so it need not be re-checked

- `x1`/`x2` (right/wrong) produce **byte-identical** JIT code (same SHA256) - the bug
  is not in the generated instruction stream
- both generated functions have exactly one `leave; ret`, stack cleanup is correct
- `rbx`/`r12`-`r15` appear **0 times** in the whole image - no callee-saved register
  is used or clobbered
- frame size is `sub $0x650` with and without pins - pinning does not change it
- `get_reg` never returned a pinned register in the failing run (diagnostics added and
  observed) - the collision theory is dead
- trigger, minimal: `var x = <expr containing a call> <arithmetic>;` breaks it;
  `var x = date.now();` alone does not; `var x = t0 - 1;` (no call) does not

Diagnostic tooling now in place: `WHALE_JIT_OBJ=<dir>` dumps `text_section` to `.bin`
for `objdump -D -b binary -m i386:x86-64`, and `WHALE_PIN_DBG` reports every
allocation with its function name and the mask.
