# The fallback crash: reproducible recipe, and where the fault is not

Follow-on to `notes/whale-step-investigation.md`. That file has the history and
the reasoning; this one has the exact reproducer and the boundary, so the next
attempt starts from the same point instead of rediscovering it.

## Getting back to the reproducer

1. Restore the work diff. `temp/whale_step_work.patch` is UTF-16 (written with
   `git diff > file` under PowerShell); convert it **and its line endings** and
   apply it with rejects:

   ```
   $p = "temp/whale_step_work.patch"
   $t = [IO.File]::ReadAllText($p, [Text.Encoding]::Unicode) -replace "`r`n", "`n"
   [IO.File]::WriteAllText("temp/whale_step_lf.patch", $t, (New-Object Text.UTF8Encoding($false)))
   git apply --reject --whitespace=nowarn temp/whale_step_lf.patch
   ```

   Two rejects are expected and both are ignorable: `JS_WhaleArrNew` (the patch
   carries an older diagnostic block) and `loop_begin` (the patch carries the
   pre-fix body; the committed fix is better).

2. **The patch does not include `src/whale_jit.h`.** It was saved with
   `git diff src/quickjs.c src/whale_jit.c`, so the eight `JS_WhaleStep*`
   declarations have to be re-added by hand or the build fails with
   `'JS_WhaleStepPushSlot' undeclared`.

3. `xmake build whalejs`, then run the reproducer.

## The reproducer

    function h(n) {
        var out = [];
        var t = { v: 1 };
        for (var i = 0; i < n; i++) out[i & 7] = t;
        return out[0].v;
    }
    h(300);
    var c = 0; for (var r = 0; r < 5; r++) c += h(3000);
    console.log("h check=" + c);

Baseline: `h check=5`, matches node. With the wiring: TinyCC refuses the
function, the interpreter runs it, and the answer is still `h check=5` - the
reproducer shows the **bad code generation**, not the wrong answer.

A two-array variant crashes instead (`A check` never prints, exit 0xC0000005);
in that one TinyCC accepts the code and the damage happens at run time.

## What the diagnostics say

With `WHALE_STEP_DBG=1`, the label sequence emitted for the reproducer is

    [label] pos=14
    [label] pos=31
    [emit] called op=72 pos=36 sp=3      <- put_array_el, handed over
    [label] pos=41
    [emit] called op=64 pos=44 sp=1      <- get_field, handed over
    whale_jit.c:1: error: pointer expected

and the same sequence repeats for the second compilation, ending in
`duplicate label 'L14'`.

So:

- **The generator emits every label exactly once.** `L14` is not emitted twice
  by us - `14`, `31` and `41` each appear once per compilation.
- The first, real error is **`pointer expected`**, raised after the `get_field`
  fallback at pos=44. In TinyCC that message means `decl()` met an identifier
  followed by `(` where it expected a type name: some emitted construct parses
  as a declaration.
- `duplicate label 'L14'` is downstream of that: with the declaration error, the
  function body is not delimited the way TinyCC expects, so the second
  compilation's `L14` collides with the first's.
- The `WHALE_JIT_SRC` dump is **not** usable to find it. It renders the fallback
  calls as well-formed `if (!JS_WhaleStepPushNum(ctx, i, _ws.v[1])) { *deopt = 1;
  ...; return 0; }` blocks, and it drops the operators the parser actually
  receives - which is exactly the information needed here.

## The next tool to build

The dump is the wrong instrument; the token stream is the right one.
`JS_WhaleTokDump()` in `src/tcc/whale_tcc_iface.c` already exists and reconstructs
the stream with `get_tok_str()`, but it is **known broken for numeric literals**
(`TOK_CINT` / `TOK_CDOUBLE` are looked up in the identifier token table and read
out of bounds - it is why an earlier investigation concluded "the stream stops
after `v0 = `" when it did not). Fix that first: it is a small change to how those
two cases are printed, and it turns this from guesswork into a diff.

Then compare, at pos=44, the token sequence the generator emits against the
sequence for the surrounding instructions. The suspicion worth testing first is
that `whale_step_emit` calls `fold_materialize()` - which emits a **statement** -
from inside a position where the surrounding generator code still has an
expression open, so the statement lands mid-expression. The `pointer expected`
message is what that looks like from TinyCC's side.

---

## Update: the fence taken down, and what it was really hiding

The "a value position must be a number" rule in whale_step_emit - the `i > 0 &&
!whale_type_is_num(t)` test - was removed to see the real failure. Its stated reason no
longer described the code: only numeric operands are materialised there now, and a
non-numeric one is copied across by slot number, so the fence had gone stale.

Result: **TinyCC accepts the generated code.** The failure is at *run time*.
`temp/w_step.js` (the first reproducer above) and `perftest/cases/nested2d.js` both die
with exit 0xC0000005, and in the reproducer that happens after the first call has
returned - not during it. So the guess in "The next tool to build" above is not what
this is: a statement landing in an open expression would be a compile error, and there
is none. The damage is in the run-time hand-over.

With `WHALE_STEP_DBG=1` the reproducer prints:

    [step] pushSlot i=0 slot=0 src_tag=-1 dst=...df20
    [step] pushSlot i=2 slot=2 src_tag=-1 dst=...df40
    [step] run pc=36 op=72 np=3 nu=0 ssize=3
    [step] put_array_el tags=-1/0/-1 stack=...df20 needs_obj=1
           var_refs=...e110 objslots=...df50 var_buf=...def0 varc=3

Ruled out, with the evidence that ruled each out:

- **cell mapping.** whale_step_cell returns `stack + i`, and whale_step_stack returns
  `sf->var_buf + b->var_count` = `def0 + 3`. var_buf is `JSValue *`, so that is
  16 * 3 = 48 bytes, i.e. `df20`. `i=2` landing on `df40` is correct.
- **operand layout.** stack[0..2] occupy `df20..df50`; the object-slot area starts at
  `df50`. No overlap, and 3 operands is the documented maximum for this path.
- **the operands themselves.** Tags are object / int / object - exactly what
  `out[i & 7] = t` should have.
- **the call.** `JS_SetPropertyValue` is declared `(ctx, this_obj, prop, val, flags)`
  and the call matches it.
- **the hand-over on its face.** `JS_WhaleStepPushSlot` does `js_dup`, and
  `JS_WhaleStepClear` frees exactly that, so it is balanced as written.

What is left is the hand-over itself. The reproducer survives its first call (300
iterations) and dies once the second begins, which is the shape of an accumulating
corruption rather than one bad access - so the suspect is an ownership mistake that only
bites when a value is stored and something else frees it later.

Next instruments, in order of directness:

1. Build the MinGW target with `-fsanitize=address` if a build can be arranged. It names
   the faulting access instead of leaving it to inference, and this is the class of bug
   it is good at.
2. Otherwise instrument the single instruction: log every js_dup and JS_FreeValue on
   stack[0..3] and on the object slot the value came from, across two calls, and look for
   a tag that dies once too often.

Nothing from this was committed: the fence was reverted and the tree is green at
`b5cfe16`.

### Update 2: the reproducer, shrunk, and one more thing ruled out

Two calls are enough - no for-loop, no closure, no warm-up:

    function h(n) {
        var out = [];
        var t = { v: 1 };
        for (var i = 0; i < n; i++) out[i & 7] = t;
        return out[0].v;
    }
    var c = 0;
    c = h(N);
    c = h(N);

    N <= 64   -> exit 0
    N >= 300  -> exit 0xC0000005

A threshold between 64 and 300, on a loop that writes only 8 array slots - so this is
the same object stored over and over, and the count at which it breaks has the shape of
a GC threshold rather than a fixed-size overflow. The second call is not special in
itself; it is that the count crosses the line there.

Also ruled out since Update 1:

- **the slot write.** `get_loc` on an object takes `JS_WhaleObjSlotCopy`, and that does
  `js_dup` before `JS_WhaleObjSlot` takes the reference over - balanced, and only when
  `src != dst`, which is right.

So the suspect list is now: the per-iteration store itself (`JS_SetPropertyValue`
taking over `stack[2]`, which was a `js_dup` - if the array does not in fact take that
reference, or takes it and the value is also freed, the count drifts by one per
iteration and a GC pass later walks a slot that should be empty).

How to reach the reproducer without editing this file: `WHALE_VALUE_OBJ=1` turns the
value-position rule off. The rule itself is unchanged by default.

---

## Update 3: the argument path - the order question, and why it does not pay yet

Raised by the user, and it is a real gap: an object laid out by one compiled function
arrives in *another* new compilation as an ordinary JSObject, and it should be laid out
there rather than degrade to a JSValue slot. Measured, same shape:

    crossfn (object passed across a call)   159ms
    objfields (object made in the same fn)   64ms
    sv[] writes: 0        ObjFieldPtr: 13       ArgGetObj: 2

`OP_get_arg0..3` parks such an argument as `WHALE_VAL_T(arg_obj_slot[idx])`, and the
field accesses then go through `JS_WhaleObjFieldPtr` - the map path, ~7 memory
operations per field. `args_obj[idx]` already records exactly the right condition (the
argument is the receiver of a field access), and the shape-based read the user pointed
at is `JS_WhaleObjFieldNum`, which is what `put_loc` already uses.

Three things came out of trying it.

**The order question was wrong.** I first reported that the layout could not be known at
`get_arg` time because the site table fills up as the generator walks forward. It does
not: `whale_jit_gen` has a preceding scan pass - it collects `field_sites`, `args_obj`,
call positions, back edges - that finishes before the emit loop starts. So the layout is
available at any point, and the "顺序障碍" I wrote last round should be struck out.

**Pre-deriving the layout for the whole function breaks OP_object and put_loc.** Doing it
right after the scan and setting `whale_str_n` makes both skip their blocks - and those
blocks do more than derive the layout: they retype the local, drop the fold cache and
commit the load. `objfields` went 64ms -> 168ms, with the object never retyped at all.
Deriving it lazily inside the `get_arg` branch fixes that (`objfields` back to 75ms).

**It is still a net loss in this shape.** With the lazy derivation in place:

    crossfn              159ms -> 578ms
    sv[] writes: 8       ObjFieldPtr: 2

The layout does happen, and it is 3.6x slower. The reason is the boundary: `run()` calls
`use(o, n)` once per loop iteration, and a struct cannot cross the call. `run`
materialises it (8 x JS_WhaleObjDefineNum) and `use` lays it out again (8 x
JS_WhaleObjFieldNum) - 16 engine calls per iteration, to save ~6 memory operations per
field access in a loop that runs 200000 times. The trade is upside down here.

So laying out an argument is correct but not sufficient on its own. It needs one of:

- the callee inlined, so there is no boundary to cross, or
- the laid-out form crossing the boundary as columns instead of through a JSObject.

Until one of those exists, the JSValue path is the better number. Reverted; the tree is
green at ebff5e4.

---

## Correction to Update 3 (the user was right on both counts)

**The 578ms was the interpreter, not a boundary cost.** The user suspected it and the qjs
control confirms it:

    qjs (pure interpreter)   crossfn   584ms
    whalejs (reverted)       crossfn   163ms
    whalejs (with the change) crossfn  578ms   <- level with qjs

Update 3 attributed it to "16 engine calls per iteration to save 6 memory operations",
which was a story invented from the wrong premise. I did not run the qjs control before
writing it, which is the exact mistake this file exists to prevent.

**What it actually is: the change made the function give up compiling.**

    [mod] pos=10 sp=3 ta=26 tb=7 struct=1/0
    gen gave up at pos=10 op=155 (mod) bail=4319
    compile failed (gen_ok=0)

`ta=26` is WHALE_T_STRUCT - so `op=155 (mod)` found the laid-out object where it expected
the operands of `i % 7`. OP_mod refuses a non-numeric operand with `goto out`, and one
refusal drops the whole function to the interpreter. With WHALE_NO_UNBOX=1 the same
function compiles (`whale_jit_fn_1`), which pins it on the unboxing.

So the argument path is not "correct but not worth it". It is **broken**: laying an
argument out changes what sits at `sp-2` for the next instruction, and the generator's
stack accounting for that instruction was written assuming the JSValue form. The stack
position the struct takes is not where the following opcode looks for its operand.

**And the user's second point stands too:** a materialisation at a compiled-to-compiled
boundary makes no sense. The laid-out form should cross as columns (or through a single
accessor that serves both the struct and a JSObject), not round-trip through a JSObject.
Update 3's framing of that as an inherent trade-off was wrong.

Next: find where the argument's struct perturbs `sp`/`cur` that the next opcode reads, and
fix the accounting rather than the symptom. The `[mod]` diagnostic that produced the line
above is worth keeping while that is done.

---

## Update 4: the argument path, second attempt - one real bug fixed, another exposed

Two things came out of trying again, and only one of them is a keeper.

**`cur[idx]` is not the argument's local slot - it is the argument *number*.** The first
attempt retyped `cur[idx]` along with the stack copy, on the assumption that index i in
`cur` is where argument i lives. It is not. In `use(o, n)` the local `i` also sits at
index 0, so the write retyped an unrelated local as a struct, the following `mod` found a
struct where it expected a number, and `goto out` dropped the whole function to the
interpreter - which is what the 578-604ms was, and why `WHALE_NO_UNBOX=1` made it go away.
Retyping only the stack copy removes the give-up entirely. **This part is correct and
worth keeping.**

**But the answer is then wrong, and that is worse.** With the give-up gone:

    [arglay] pos=7 idx=0 slot=0 n=4 sp=0
    crossfn 433ms check=0
    node    20ms check=4799933

Every field reads back as 0. Either `JS_WhaleObjFieldNum` returns 0 without the caller
being told (its `*deopt`) and the layout is silently zeros, or the `sp[]` presence flag is
not being interpreted the way the field reads expect. This is a silent wrong answer, so
the whole change is reverted rather than left in - a function that used to be slow and
right is now fast and wrong.

**The remaining shape of the job**, for whoever picks it up:

- `cur[idx]` must not be written; only the value on the stack may be retyped (done, kept
  in this note).
- `JS_WhaleObjFieldNum`'s failure path has to be checked at the call site: if it can fail
  without setting deopt, the layout fills with zeros and the reads return them.
- 433ms against 163ms says re-laying-out the argument on every `get_arg` is still too
  expensive even when it compiles - which is the trade the user rejected anyway: a struct
  should cross the boundary as columns, not be rebuilt on each read.

The `[arglay]` and `[mod]` diagnostics (both gated on WHALE_JIT_DUMP) stayed in during this
work and are what produced the two lines above.

---

## Update 5: (2) checked - JS_WhaleObjFieldNum's failure path is fine; and the design point

**The failure path is correct.** `JS_WhaleObjFieldNum` ends with `bad: *deopt = 1; return 0;`
and every refusal jumps there - wrong tag, wrong class_id, property not found, an accessor
(which would have side effects). So the caller *is* told. `check=0` is not a swallowed
failure.

**The shared shape cache was ruled out by measurement, not argument.** The next theory was
that `*shape_cache`/`*ptr_cache` hold "shape X, some field at Y" rather than a per-atom
result, so giving four atoms the same cache slot would make the last three read field 0.
That does not happen:

    { a: 11, b: 22, c: 33, d: 44 }   whalejs 110 / node 110 / qjs 110

and `put_loc`'s existing layout (whale_jit.c:3565) uses exactly that shared slot
(`&_ws.fs[WHALE_FIELD_CACHE - 1]`) and is correct on `objfields` too. So the cache is not
the cause of `check=0` either, and the same mistake in the argument path would not have
produced it. Whatever made the argument-path run return 0 is something else - the nearest
candidate is the interaction between `arg_obj_slot` and when the layout is filled relative
to `sp[]`, not the read itself.

**The design point, which supersedes "3" from the last round.** I described the boundary
problem as though a laid-out struct had to be rebuilt at every read - "correct but not
sufficient" - and the user corrected it: that is not a constraint, it is a choice, and the
wrong one. The intended shape is:

- the object stays a `JSObject`; it is one object with one identity, exactly as now
- that object carries a pointer to a C struct - the native form - inside it
- the JIT's machine code operates on that struct directly
- the interpreter, getters and everything else keep seeing the `JSObject` and pay one
  extra check to notice a native form exists

One object, two views, no materialisation and no re-derivation at any boundary. Extending
`JSObject` with the native pointer is the user's proposal and it removes the entire class of
problem this note has been circling. Inlining is still valuable, but it is not the only way
through - that was my framing error, not a fact.

Nothing from the argument-path attempt is in the tree; it is reverted at fa1f6ac, green.
