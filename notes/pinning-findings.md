# Pinning: two findings, one fixed, one open

## FINDING 1 (fixed in this round's working tree, not committed): whale_nloops was always wrong

```c
for (k = 0; k < len; k++)
    if (loop_back[k])          /* loop_back[] holds -1 for "no back edge" */
        nloops++;
```

`loop_back[]` is initialised to -1 and set to the back-edge position otherwise, so
`if (loop_back[k])` is true for **every** entry: `arith` has one loop and reported
`nloops = 21`. The generator's pin gate is `!whale_has_call && whale_nloops <= 1`,
so with nloops == len it was effectively **off** for every function longer than a
couple of bytes. That is the real reason no pin ever took effect in a loop.

Fix: `if (loop_back[k] >= 0)`.

With that, arith/branch/bitops report `nloops=1` and get pins; minmax reports
`nloops=2` and is correctly excluded (it hangs if a counter in a nested loop is
pinned - pinning its accumulators lo/hi is fine, which is what the third pin slot
used to do by accident).

## FINDING 2 (open): reading order changes the result

With pins active, this is deterministic and reproducible:

    function build(n){ var a=[]; for(var i=0;i<n;i++) a[i] = i % 97; return a; }
    function scan(a){ var s=0; for(var i=0;i<a.length;i++) s += a[i]; return s; }
    scan(build(1000));
    var t0 = Date.now();
    var check = 0;
    for (var r=0;r<20;r++) check = scan(build(50000));
    var ms = Date.now() - t0;
    console.log("check=" + check + " ms=" + ms);      // -> check=406 ms=0

`check` should be 2398830. What varies is not the generated code - dumping the
generated C for the passing and failing versions gives byte-identical token streams,
differing only by the two interleaved output lines. What varies is **which local is
read first**:

    console.log(check); console.log(check);          -> 2398830 / 2398830   OK
    console.log(ms);    console.log(check);          -> 0 / 406             WRONG
    console.log(check); console.log(ms);             -> 406 / 0             WRONG
    console.log(check * 1); console.log(ms * 1);     -> 2398830 / 142       OK

So: read `check` first with nothing before it, and it is right. Touch anything else
first, and `check` comes back as a stale value. Multiplying by 1 refreshes it.

That shape - correct only if read first, wrong after any other access, fixed by a
single arithmetic operation - means a copy of the value survives in a place that is
not refreshed. The generated code is the same, so the difference is in run order, and
`check`/`ms` are locals of the **top-level** script, which is interpreted, not
compiled. So the corruption comes from `build`/`scan` (both pin `i` and `s`) leaving
something behind that the interpreter then reads as `check`.

Both are pinned with `pin1=L1` (`i`) and `pin2=L0` (`s`), and `s` is the **return
value** - this is the same shape the commit 7d9247b message describes: "Return values
are asked for in a specific register (RC_IRET/RC_FRET) and returning the pinned one
unconditionally left the caller reading a register the value was never in - `return s`
came back as the loop counter." That was fixed once for the old pin condition; the
new one (pin `s` as well as `i`) reintroduces a variant of it.

Next: dump the generated C for `scan` alone and look at the return sequence against
the pin at tccgen.c:1411, and check whether the two pinned registers are still
holding `scan`'s values when the caller in the interpreter reads its own locals.

## State

Reverted to 38579bc: 24 cases, no generation errors, no mismatches, 166/137 timings.
The nloops fix and the selection rules are in git stash/working tree only.
