# Debugging discipline: what went wrong, and how to catch it next time

## The failure worth remembering

Commit `070a5ee` changed one line - `sym->r = reg` for a pinned local - and reported
"all 24 cases match node, timings unchanged within noise". Both statements were
true. Both were also true of a build where **every JIT compilation failed** and every
benchmark silently fell back to the interpreter.

The suite reported 0 mismatches because the interpreter agrees with node. The timings
were unchanged because 0 ms of JIT and 0 ms of JIT is the same number. I read those
two signals as confirmation.

**`0 mismatch does not mean the JIT worked. It means the answers were right, and the
interpreter is very good at being right.** Every benchmark run in this repo needs a
generation-error check beside it:

    foreach case: whalejs case.js 2>&1 | Select-String "lvalue|invalid macro|: error|expected"

The compiler's errors are swallowed - the JIT gives up, the interpreter takes over,
and the program still prints the right answer at 10-20x the time. `gen-errors=0/24`
belongs next to the mismatch count, always.

## Patching symptoms instead of understanding

The pinned-register mechanism grew six sites in tccgen.c (953, 1056, 1408, 2276,
3331, 4277), each a hand-written special case for one access path. One of them
literally says "Without this the pin did nothing but waste a register" - a
description of the last hole, not of the cause.

The cause was that `sym->r` was never changed, so the symbol table believed the local
lived in its frame slot while the value was in a register. Every access then needed a
patch. TinyCC already has the correct mechanism for exactly this - it is what
`asm("reg")` uses at tccgen.c:7092: point `sym->r` at the register and let gv()/store()
do the rest.

The six patches are not just redundant, they are **load bearing against the fix**:
they rewrite `vtop->r` to `VT_LOCAL`, which contradicts `sym->r`. That is why setting
`sym->r` alone broke code generation - the patches fought it.

**Rule: if a fix needs a special case on every access path, the model is wrong. Find
the field that should be authoritative and let the existing machinery use it.**

## Auditing for this class of mistake

The signatures are greppable:

- `WhaleJS:` comments in TCC sources - each one is a place we diverged from upstream
- comments saying "without this", "otherwise", "work around", "avoid" - each is a
  hole that was plugged rather than understood
- a state variable mirrored in two places that must be kept in sync by hand

    Select-String -Path src/tcc/*.c -Pattern "WhaleJS:|without this|otherwise|work around|avoid"

That found the pin cluster (six sites) and nothing else of comparable shape. The rest
of the WhaleJS changes in TCC are additive (a new allocation routine, a new reset
hook) rather than corrections of existing behaviour.

## What the pin work actually amounts to

Two things are known and verified independently of the broken attempt:

1. **Pinning pays 25-30% when the right locals are chosen** (`locals` 112->80,
   `branch` 123->90, `nested` 101->74, `ident` 66->55). An earlier reading of "no
   gain" was the allocator being handed the wrong two locals.
2. **Two pins is the safe maximum.** A third hangs: get_reg() cannot reclaim a pinned
   register, finds nothing, and returns -1, which its callers treat as impossible.

So the work is real and bounded. What it needs is the mechanism fixed first - the six
patches removed and `sym->r` made authoritative - and only then the selection
(weighted `inc_loc`/`dec_loc`) and the reach (`nloops` restriction).

## Next time, in order

1. Read the path before changing it. For this task: everywhere `sym->r` is consulted
   (parsing, vset, gv, store) for a local whose `VT_VALMASK` is a register.
2. Make the change once, with the patches removed, not incrementally around them.
3. Check `gen-errors=0` **and** the mismatch count **and** the timings.
