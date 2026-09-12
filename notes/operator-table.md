
## The operator table was one entry short of its enum (found, not fixed)

`JS_WhaleTokOp` (src/tcc/whale_tcc_iface.c) maps a `JS_OP_*` value to a token
through a hand-written table:

```c
static const int optok[] = { '+', '-', '*', '/', TOK_LT, TOK_LE, TOK_GT, TOK_GE,
                             TOK_EQ, TOK_NE, TOK_LOR,
                             TOK_SHL, TOK_SAR, TOK_SHR, TOK_ARROW, '.' };
```

That is 16 entries. `JS_OP_*` has 17: `JS_OP_AND` is missing. Everything from
it on is therefore shifted by one, and the last value falls off the end:

| value | emitted | should be |
| --- | --- | --- |
| `JS_OP_AND` | `<<` | `&&` |
| `JS_OP_SHL` | `>>` | `<<` |
| `JS_OP_SAR` | `>>>` | `>>` |
| `JS_OP_SHR` | `->` | `>>>` |
| `JS_OP_ARROW` | `.` | `->` |
| `JS_OP_DOT` | *(one past the end)* | `.` |

`JS_WhaleTokOp` bounds-checks with `o < sizeof(optok)/sizeof(optok[0])`, so
`JS_OP_DOT` pushes **nothing** - the read never happens. That is where the empty
parentheses in ident.js came from:

```c
L0 = ((((L0 / ()) - ()) + _ws.v[...]));
```

Field access emits `->u` + `JS_OP_DOT` + `float64` (line 838), so a missing entry
there leaves `->u float64`; it compiles because the `(` that opens the ternary arm
and the `)` that closes it end up adjacent.

The reason all 24 cases still matched is that `JS_OP_OR` and `JS_OP_AND` are used
**only as condition joiners** (lines 820-891, 2198-2482, 3159 - every one is inside
an `if (...)`), and in a `?:` or `if` condition any non-zero value is as good as
1, so `(x >= A) << (x <= B)` behaves like `(x >= A) && (x <= B)`.

### The attempted fix, and what it exposed

Adding the missing `TOK_LAND` entry (17 entries, everything aligned) does fix the
emitted C - but `bitops` then reports `56575` where node says `60479`.

So aligning the table is **not sufficient on its own**, and the remaining
discrepancy is real: `bitops` exercises `^ << >> | & %` in one loop and one of
those is still emitted wrongly. The operators are the place to look, not the table
layout. Note also that an earlier attempt which *also* changed `JS_OP_OR`/`AND` to
`'|'`/`'&'` made `bitops` come out **correct** (60479) - which is itself the clue:
something in this path expects the bitwise spelling, and the condition sites expect
the logical one. One enum value cannot serve both, so the joiner needs to be
separate from `JS_OP_OR`/`JS_OP_AND` before the table is aligned.

Reverted to `32dcd44`; all 24 case checks match node again.
