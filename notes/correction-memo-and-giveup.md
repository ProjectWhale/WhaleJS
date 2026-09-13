# Correction: the "gen gave up" finding was misread, and the 27x was a memo artifact

Two separate wrong conclusions had been built up, and both are now disproved by
measurement. Recording them so they are not re-derived.

## The 27x gap between objfields and object does not exist

`object` looked 27x faster (6ms against 161ms). It is not faster; it was measured
against a memo.

object.js is `for (r = 0; r < 20; r++) check = tally(200000)` - the same argument
every time, and `tally` is pure (no calls, no global reads), so the engine's memo
table short-circuits nineteen of the twenty calls. objfields cannot do that, because
`run()` calls `make()` and is therefore not pure.

Measured with arguments that change, so the memo cannot fire:

    object(varying)   147ms
    objfields         166ms

Same order. Both are about 40ns per iteration, or roughly 130 cycles, for four field
operations. There is no regression to chase here, and objfields is not a special
"heavy" case - a factory function plus a field-update loop is simply ordinary code.

This is the third time a fixed-argument repeat has produced a wrong number in this
work. temp/measure.ps1 takes the minimum of N runs but does NOT vary arguments; a
case whose loop calls the same pure function with the same argument every time must
be varied by hand.

## "gen gave up" is a fallback, not a failure

`whale jit: gen gave up at pos=13 op=66 (put_field)` appears for objfields, and the
generated source contains no `run()` - so it was read as "run is never compiled, it
runs in the interpreter". That is wrong.

`whale_on_call` calls `JS_WhaleJitCompileFunction` TWICE (quickjs.c:3878 and 3887).
The engine compiles two readings of a function, because a closure slot read as a call
target may or may not be this very function. The first reading fails here; the second
succeeds:

    [gen] argc=1 varc=2 len=102
    [put] pos=13 ta=1 slot=0        <- reading A: the object is not a slot yet
    whale jit: gen gave up at pos=13 op=66 (put_field)
    [gen] argc=1 varc=2 len=102     <- same function, reading B
    [put] pos=13 ta=56 slot=1
    whale jit: whale_jit_fn_2 -> ...   <- compiled

A function called exactly once, with all its heat from the loop inside it, is the case
that would have no second chance. It was tested directly (temp/probe/once.js) and it
still compiles:

    called once, object from make()   436ms
    same but with an object literal   440ms

So the call-built object costs nothing measurable either. `ta=1` on the first attempt
is real, but it is the first reading's tracking, and the second reading has it right.

## What was actually learned

- The generated field access is identical in shape for both cases (same
  `_ws.fv[fc] != fver || _ws.fs[fc] != JS_WhaleObjShape(...)` condition), which is why
  none of the `stable` / observation work could move the number.
- The images are nearly the same size and call count (object 5354 bytes / 48 calls,
  objfields 5834 / 53).
- The real cost is ~130 cycles per loop iteration for four field operations, which is
  where any further work has to aim.

## Still true and worth keeping

- The instruction-set observations, `whale_prof_*` and `JS_WhaleProfShapes`, are
  correct and inert (nothing depends on them for speed).
- Pinning into xmm6/xmm7 is correct and marginal.
- The general fallback the user asked for (one instruction dispatched into the VM
  instead of abandoning the function) is still not implemented. The evidence above
  does not show it buying anything on these cases, because the two-reading mechanism
  already recovers from a failed first attempt - but a case where BOTH readings fail
  has not been found or ruled out yet.
