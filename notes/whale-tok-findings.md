# Two things found with a working token-stream dump

Follow-on to `notes/whale-step-repro.md`. That file's "next tool to build" was a
usable view of the token stream. It turned out the dump already existed and
worked - what was missing was the **position**. Both halves came out of one
change, and one of them is a real generator bug.

## 1. TinyCC's error position now points into the token stream

`JS_WhaleCompileTokens()`'s `setjmp` path in `src/tcc/whale_tcc_iface.c` now
prints `whale_tok_pos` and the surrounding tokens (`[tokerr] failed at token N of
M:` plus eight before and four after, with the failing one marked `<<<`).

That is the instrument the whole search needed. Before it, the only clue was
`whale_jit.c:1: error: pointer expected` - line 1, because the unit is fed as
tokens from a hook and the file has no real lines.

## 2. A real generator bug: `->` where the operand is a value

`whale_jit.c`, the array-element unboxing in `OP_get_array_el`
(the `ap[os][(int)_ws.v[tk]]` sequence):

```c
wt_arr("ap", os); wt('['); ... wt(']');
wt_op(JS_OP_ARROW); wt_id("tag"); wt_op(JS_OP_EQ); wt_int(8);   /* -> tag */
```

`_ws.ap` is declared `JSValue *ap[WHALE_OBJ_SLOTS]`, so `ap[os]` is a
`JSValue *` **but `ap[os][i]` is a `JSValue` - a value, not a pointer.** Member
access on it is `.`, never `->`. TinyCC's `decl()` saw the identifier `tag`
after `->` where it wanted a type name and reported exactly the `pointer
expected` that had been blocking the reproducer.

The same shape appears three more times in that expression (`->u.float64`,
`->tag`, `->u.int32`) and all four are now `JS_OP_DOT`. The neighbouring
`_ws.fp[fc]` accesses (object fields) genuinely are pointers and keep `->`.

**Why the baseline never noticed:** on the baseline the reproducer does not
compile at all - some earlier `goto out` stops it - so this path is never
emitted. Widening what compiles is what walked into it. That also means the bug
is **independent of the fallback**: anyone who widened the compiled set would
have hit it, and it is worth fixing on its own.

**Verified:** with the four tokens fixed, `pointer expected` is gone and the
reproducer compiles instead of falling back to the interpreter.

## What is still open

`JS_NewArray` still returns `{ tag = -1, ptr = <valid> }` (i.e.
`JS_TAG_UNINITIALIZED`) inside a JIT frame, and both reproducers still die with
0xC0000005 - now at run time rather than at compile time, which is the progress
this round bought.

`JS_NewObjectFromShape()` calls `js_trigger_gc()` as its first statement
(quickjs.c:7244), and `JS_UNINITIALIZED` is QuickJS's cycle-collection marker -
so the shape of the suspicion is that an allocation inside a JIT frame triggers
a collection that treats the frame differently from an interpreted one. That is
still a hypothesis. The cheap test remains the same: call `JS_NewArray` from a
TCC-generated function in a standalone program, with no fallback machinery
involved.

## Where the work stands

`temp/whale_full.patch` (34 KB, LF, plain ASCII) is **complete** - all four files,
including `src/whale_jit.h`, which the earlier patch was missing. It applies with
`git apply` and reproduces the current state exactly. The working tree is left at
that state, deliberately uncommitted: `object`/`objfields`/`array`/`sieve`/`fib`
are correct, `objshape`/`nested2d` crash.

Two changes in that working tree are worth committing **on their own**, without
the fallback wiring, because neither depends on it:

- the four `JS_OP_ARROW` -> `JS_OP_DOT` tokens in `OP_get_array_el`;
- the `[tokerr]` print in `whale_tcc_iface.c`'s `setjmp` path.
