# Unboxing: use a function-level candidate test, not a per-local one

Deciding which access site belongs to which local needs value-stack provenance, and
`cur[]` carries a `snap[]` at every jump target - a parallel provenance array would
need the same treatment, which is where the cost is.

It is not needed. The cases with a measurement behind them are single-object
functions:

    objfields:   function run(n) { var o = make(); for (...) { o.tag = ...; ... } }
    object:      function tally(n) { var o = { a: 0, b: 0, c: 0 }; for (...) ... }

One object each. So the test can be asked at the function level instead of the local
level:

> every field access site in this function was observed monomorphic

If that holds, the objects in it are single-shaped, and the one that OP_object creates
can be laid out. The cost is that a function with two objects of different shapes is
refused - which is the conservative direction, and those are not the cases being
chased.

This keeps the existing pre-scan as the only place that has to change: it already
walks every opcode, so collecting the field-access offsets as it goes is one more arm
in a loop that already exists, with no stack simulation and no snapshots.

Recorded here because the alternative looked like more work than it is, and because if
the per-local version is ever needed, this is the reason it was not done first: prove
the function-level version on objfields, then generalise only if a real case wants it.
