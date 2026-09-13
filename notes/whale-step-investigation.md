# Mixed compilation: the single-instruction fallback, and what actually blocked it

Everything here was established by running, not by reading. The work itself was
reverted (see "State" at the end); this file is what it cost and what it found.

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
- **Generator side**: `whale_step_emit()`, a macro-free helper taking
  `(op, pos, &sp, cur, var_count, stack_size)`, wired into `default:` and into
  the refusals of the five cases above.

Coverage did widen as intended: `gen gave up` over the 24 perftest cases went
from **23 to 10**, and the remaining ten name instructions the engine does not
run yet (`call2`, `call_method`, `return`, `add_loc`, `get_field2`).

## The real bugs it uncovered, in order

**1. The loop buffer was writing out of bounds, and losing code silently.**

`WHALE_LOOP_BUF` is 4096, but `fold_commit()` and `loop_end()` both bound the
append with `*whale_out_len < WHALE_TOK_MAX` (32768). While a loop body is open,
`whale_out_tok` points at `WhaleLoop.tok[4096]` - so any body past 4096 tokens
wrote past the end of the array, and `loop_end()`'s `n = min(l->len,
WHALE_LOOP_BUF)` dropped the tail of anything longer without a word.

Nothing had hit it because `loop_begin`/`loop_end` have never been used for a
loop-level pass - the whole buffer is dead weight. Handing instructions to the
engine made bodies longer, and both halves showed up at once.

Fix, verified to build and to leave all 24 cases unchanged: make `loop_begin`
return 0 (the "stack is full" path that already existed), which leaves
`whale_loop_depth` at 0 forever, so `loop_end` is never entered. One function,
no new mechanism.

**2. `JS_NewArray` returns `{ tag = -1, ptr = <valid heap pointer> }` inside a
JIT frame.**

This is the one that stopped the work. Reproduced to three lines:

    function h(n) { var out = []; var t = { v: 1 }; for (var i = 0; i < n; i++) out[i & 7] = t; ... }

`objshape.js` (835ms, interpreted) and `nested2d.js` die with 0xC0000005;
`escaped`, `twoShapes`, `writeOrder` all go through the same shape. The trace:

    [step] arrNew slot=0 slots=0x...570 a.tag=-1 a.ptr=0x...810 a.tag32=-1 pre=3
    [step] arrNew post tag=-1 at 0x...570

So the slot writing machinery is fine (`pre=3` is `JS_UNDEFINED`, correctly
initialised), the array object really was allocated (`a.ptr` is valid), and the
**returned tag is `JS_TAG_UNINITIALIZED`**. `JS_IsException()` does not catch
that, so it is stored and handed to `JS_SetPropertyValue` as an object, which
crashes.

`JS_NewArray` itself is three lines (`JS_NewObjectFromShape(ctx,
js_dup_shape(ctx->array_shape), JS_CLASS_ARRAY, NULL)`, quickjs.c:7484) and is
called successfully by the interpreter in the same process a hundred times
before the JIT entry exists.

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

## Method notes that cost time

- `getenv("WHALE_JIT_SRC")`'s C dump is **not** a reliable view of the emitted
  stream: the `JS_WhaleStep*` calls that demonstrably ran never appeared in it,
  only the prototypes. Do not conclude "it was not emitted" from that file.
- `WHALE_STEP_DBG` printed reliably from the engine helpers but not from
  `whale_step_emit`'s own `dbg` flag on the compile-time path. When a diagnostic
  does not appear, suspect the diagnostic before the mechanism.
- One editing mistake here (`old_string` matching one line too many) deleted
  `{ int fc = whale_fields++;` and broke the build. Read the replacement result
  back before moving on.

## State

Reverted to the last green commit. The full working diff was saved to
`temp/whale_step_work.patch` (58 KB: engine-side executor + generator wiring +
the diagnostics) - it is a starting point, not a result: it makes 10 more cases
compile and makes `objshape`/`nested2d` crash.

`temp/` is not committed, so that patch is the only copy. If it is to be
picked up again, the first question is **#2 above**: why does an allocation
inside a JIT frame hand back `JS_TAG_UNINITIALIZED` with a valid pointer. That
is answerable with one small test program calling `JS_NewArray` from a
generated TCC function, without any of the fallback machinery.
