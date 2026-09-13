# Mixed compilation: the single-instruction fallback, and what actually blocked it

Everything here was established by running, not by reading. The work itself was
reverted (see "State"); this file is what it cost and what it found.

> **Correction (later round).** An earlier revision of this note called
> "`JS_NewArray` returns `JS_TAG_UNINITIALIZED`" the root cause. It is not. It is a
> **symptom of memory corruption introduced by the fallback wiring**: adding one
> unrelated `JS_NewObject(ctx)` call inside `JS_WhaleArrNew` makes the crash and the
> bad tag disappear entirely (`A check=66`, matching node), and the same three-line
> shape is correct on the baseline. The `duplicate label` error TinyCC reports is
> likewise absent on the baseline. Read the `JS_NewArray` section below as an
> observation, not a diagnosis.

## What was built

The general fallback of `notes/mixed-compilation-boundary.md`, end to end:

- **Engine side** (quickjs.c, after `short_opcode_info`): `JS_WhaleOpArity`,
  `JS_WhaleStepCovers`, and a single-instruction executor
  `JS_WhaleStepRun(ctx, pc_off, n_pop, n_push)` with
  `JS_WhaleStepPushNum` / `PushSlot` / `OutIsNum` / `OutNum` / `OutSlot` /
  `Clear` for moving the operands in and out. The operand stack it uses is the
  interpreter's own (`sf->var_buf + b->var_count`), so no second
  representation of the frame is involved. Covered: `push_empty_string`,
  `push_atom_value`, `get_field`, `get_length`, `put_field`, `get_array_el`,
  `put_array_el` - each checked against `short_opcode_info` arity before use.
- **Generator side**: `whale_step_emit()`, taking
  `(op, pos, &sp, cur, var_count, stack_size)`, wired into `default:` and into
  the refusals of the five cases above.

Coverage did widen as intended: `gen gave up` over the 24 perftest cases went
from **23 to 10**, and the remaining ten name instructions the engine does not
run yet (`call2`, `call_method`, `return`, `add_loc`, `get_field2`).

## The bugs it uncovered

**1. The loop buffer was writing out of bounds and losing code silently - fixed
and committed (`86e5a52`).**

`WHALE_LOOP_BUF` is 4096, but `fold_commit()` and `loop_end()` both bounded the
append with `*whale_out_len < WHALE_TOK_MAX` (32768). While a loop body is open,
`whale_out_tok` points at `WhaleLoop.tok[4096]` - so any body past 4096 tokens
wrote past the end of the array, and `loop_end()`'s `n = min(l->len,
WHALE_LOOP_BUF)` dropped the tail of anything longer without a word.

Nothing had hit it because `loop_begin`/`loop_end` have never been used for a
loop-level pass - the whole buffer is dead weight. Handing instructions to the
engine made bodies longer, and both halves showed up at once.

Fix, verified to build and to leave all 24 cases unchanged: make `loop_begin`
return 0 (the "stack is full" path that already existed), which leaves
`whale_loop_depth` at 0 forever, so `loop_end` is never entered.

**2. Memory corruption from the fallback wiring - NOT fixed, not localised.**

The symptom chain, in the order it was observed:

- `objshape.js` (835ms interpreted) and `nested2d.js` (9ms) die with 0xC0000005.
- Inside the JIT frame `JS_NewArray(ctx)` appears to return
  `{ tag = -1, ptr = <valid heap pointer> }`: the array object is really built,
  the tag reads as `JS_TAG_UNINITIALIZED`. `JS_IsException()` does not catch `-1`,
  so the value is parked in an object slot and handed to `JS_SetPropertyValue` as
  an object. Trace:

      [step] arrNew slot=0 slots=0x...570 a.tag=-1 a.ptr=0x...810 a.tag32=-1 pre=3
      [step] arrNew post tag=-1 at 0x...570

  (`pre=3` is `JS_UNDEFINED`, correctly initialised - the slot machinery is fine.)
- **Adding one line - an unrelated `JS_NewObject(ctx)` in the same function -
  removes the crash and makes the program give the node-identical answer
  (`A check=66`); removing it brings the crash back.** That is a
  memory-corruption signature: the bad tag is downstream of the real fault.
- TinyCC reports `duplicate label 'L14'` on a three-line reproducer
  (`function h(n) { var out = []; var t = { v: 1 }; for (var i = 0; i < n; i++) out[i & 7] = t; ... }`)
  under the wiring; **the same reproducer on the baseline is clean**
  (`h check=5`, matching node). So the wiring does not merely emit more - it
  changes the emitted stream in a way that is not well-formed.

Where to look next: the emitted stream and the generate-time state around
`whale_step_emit`, not the engine helpers. `whale_step_emit` calls
`fold_materialize()` (which emits a statement of its own) and
`fold_mark()`/`fold_commit()` from inside a case body, where the surrounding
generator code may already have emitted a label - the `duplicate label` is the
concrete thread to pull.

## What is NOT the cause (each tested, not reasoned)

- **The `cur_sp` loan.** `quickjs.c` borrows the frame's live range
  (`sf->cur_sp = obj_slots + WHALE_OBJ_SLOTS`) so GC can see parked objects.
  Setting it to `NULL` in the JIT branch changes nothing - the crash and the
  `tag = -1` are identical.
- **Object slots as such.** `object.js`, `objfields.js` and `array.js` all use
  them (and `JS_WhaleArrNew`), and all three are correct and unchanged.
- **The frame-layout ordering bug of note <46>.** `whale_jit_ready` still
  handles the call that compiled the function, and the frame addresses are
  self-consistent: `var_buf`, `stack = var_buf + var_count`,
  `obj_slots = stack + stack_size`, `var_refs = obj_slots + 12`, `needs_obj=1`.
- **`whale_frame_obj_slots` / `JS_WhaleObjSlot` / `JS_WhaleArrNew`.** All
  three read and write correctly, and `whale_step_emit` was confirmed to run
  (`[emit] called op=72 pos=66 sp=3`).
- **"The fallback was never emitted".** It was.

## Method notes that cost time

- The `WHALE_JIT_SRC` C dump is **not** a reliable view of the emitted stream:
  the `JS_WhaleStep*` calls that demonstrably ran never appeared in it, only the
  prototypes. Do not conclude "not emitted" from that file.
- A diagnostic that does not appear is more often the diagnostic's fault than the
  mechanism's. An `getenv`-gated print added to `whale_step_emit` produced
  nothing on the compile-time path until the condition was removed.
- **`git diff > file` under PowerShell writes UTF-16**, which `git apply` rejects
  ("No valid patches in input"). Convert with
  `[System.IO.File]::ReadAllText($p, [Text.Encoding]::Unicode)` +
  `WriteAllText(..., UTF8Encoding($false))`. A
  `Get-Content -Raw | Set-Content` round trip corrupts the line endings instead.
- One editing mistake here - an `old_string` that matched one line more than
  intended - deleted `{ int fc = whale_fields++;` and broke the build. Read the
  replacement result back before moving on.

## State

Tree is green at `86e5a52` (baseline + the loop-buffer fix); `whale api test: ok`;
24 perftest cases correct. The fallback work is **not** in the tree. It exists as
`temp/whale_step_work.patch` (UTF-16, as written) and
`temp/whale_step_work_u8.patch` (same bytes converted to UTF-8, confirmed
parseable by `git apply --check`). Applying it against `86e5a52` conflicts in
exactly two places, both known: `JS_WhaleArrNew` (the patch carries an old
diagnostic block) and `loop_begin` (the patch carries the pre-fix body).

`temp/` is not committed.
