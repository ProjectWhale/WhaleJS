# RETRACTED - the operator table is correct

An earlier version of this file claimed `JS_WhaleTokOp`'s `optok[]` table was one
entry shorter than `JS_OP_*`, that `JS_OP_AND` had no entry, and that `JS_OP_DOT`
fell off the end. **That was wrong.**

`optok[]` has 17 entries and has always matched the enum:

```c
static const int optok[] = { '+', '-', '*', '/', TOK_LT, TOK_LE, TOK_GT, TOK_GE,
                             TOK_EQ, TOK_NE, TOK_LOR, TOK_LAND,
                             TOK_SHL, TOK_SAR, TOK_SHR,
                             TOK_ARROW, '.' };
```

`TOK_LAND` is present in `32dcd44` and in every commit before it. The reading that
started the claim came from a truncated line in a `Select-String` listing - the
`TOK_LAND` entry was cut off, not missing. A fresh `xmake -r -y` on a clean tree
gives `bitops` = 60479, matching node.

Lesson, since this cost a round: when a grep result is the evidence for a claim
about a table's *length*, print the whole table in one piece and count it. A
`-First N` or a column-truncated line is not evidence about the end of an array.

## What is still true and still unexplained

The empty parentheses are real, not a misreading. This:

```js
function f(n) { var s = 1.0; for (var i = 0; i < n; i++) { s = s / 1.0; s = s - 0.0; } return s; }
```

generates

```c
L4: ... goto L20; L0 = (((L0) - ())); L1 = (L1 + 1); goto L4;
```

`s - 0.0` should fold to `s` (the algebraic-identity path at whale_jit.c:1863) and
instead emits `(L0) - ()`. Note that `s / 1.0` *did* fold - only the `- 0.0` arm is
broken - so the identity logic does run, and the operand load inside it is what
produces nothing.

Two candidates checked and ruled out:

- `fold_inline_expr` receiving `len == 0`: its counter reads 0 across all 24 cases.
  But that counter was only exercised by the suite, never by an `-e` probe, so
  re-check it with the probe that does show `()` before trusting this.
- `JS_WhaleTokOp` bounds: the table is complete; nothing falls off the end.

## Still worth knowing

Bitwise operators do not go through `wt_op` at all - whale_jit.c:3099 writes the
character directly:

```c
wt(op == OP_and ? '&' : op == OP_or ? '|' : '^');
```

while `<<`/`>>` do go through it (3117-3121), with `>>>` spelled as an `unsigned
int` cast plus `>>`. So the generator spells the same operators two ways and only
one of them consults the table. Not a bug today, but it is why a table change looks
like it ought to matter and does not.

## Next

The identity path at 1863-1882. For `x - 0.0`, `id_keep == 0` calls
`whale_load_emit(k1, i1, cv1)` - the left operand - so it should emit `L0`.
Something is emitting zero tokens for a slot the identity path believes is loaded.
Start by making `fold_inline_expr`'s counter reachable from an `-e` probe.
