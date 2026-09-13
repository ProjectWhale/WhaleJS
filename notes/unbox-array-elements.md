# Direction: unbox array elements into native structs

Raised by the user, and it is the most promising thing on the table for objfields.

## The observation

objfields is not much better than object once the memo is defeated (147ms against
166ms, both ~4 field operations per iteration at ~130 cycles). The general fallback
has stopped moving the number, and the reason is that the fallback only removes
*give-ups* - it does not change how a field access is executed. A field access is
still:

    load _ws.fv[fc]; compare to _ws.fver; load _ws.fs[fc]; compare;
    load _ws.fp[fc];  load fp->tag; branch; load fp->u.float64 or fp->u.int32;

that is three loads just to find the value, per access, and the shape/version pair is
re-read every time. That is where the ~130 cycles go.

## The idea

If the array element type is known to be a single fixed object shape, the element does
not have to stay a `JSValue` pointing at a heap object. It can become a **C struct**:
one field per property, plain `double` (or int64) slots, no tag, no pointer chase.

    // instead of JSValue[] where each element heads a heap object with a shape
    struct VNode { double tag, key, flags, depth; };
    struct VNode *v = ...;   /* the array's backing store */

Then:
- a field access is `v[i].depth` - one load, no shape check, no tag branch
- the shape/version machinery disappears from the loop entirely, which is the point:
  it exists to guard against a shape change that cannot happen for a struct
- the array itself becomes callable from native code, because its element layout is
  a compile-time fact

The user's phrasing is the key insight: the array does not need its own machinery. It
needs its *element* to be materialised, and then the array is ordinary C.

## Why nested comes along

`nested` cannot be unboxed further - its inner values really are dynamic - but if the
outer structure is a struct array, the inner accesses become `s->inner[k]` on a known
layout instead of a generic property lookup. The user's expectation is a large gain
there; the mechanism is the same one.

## What has to be true, and how to establish it

The whole thing rests on knowing the element shape at compile time, and that is what
the observation machinery added earlier in this work actually provides:

- `whale_prof_*` / `JS_WhaleProfShapes` already record, per property access site, which
  shapes were seen during the last interpreted calls before compilation. A site that
  saw exactly one shape across a hundred calls is a candidate.
- The same question wants asking about *array elements*: during those interpreted
  calls, record the shape of each element stored into a given array. If one array only
  ever holds one shape, it can be unboxed.
- Bounded memory applies as it does now: a site remembers a fixed number of shapes and
  is abandoned if it exceeds that.

So this extends the observation table from "property access sites" to "array element
types", and adds a code path that emits a struct-typed backing store plus indexed
member access instead of the `_ws.fv/fs/fp` sequence.

## The measurement, done (WHALE_JIT_SRC on objfields)

The generated loop body, with the operators stripped out by the mirror (wt_op is not
mirrored, so they show as blanks):

    L5 : ( L1 <op> A0 ) ? 1 : 0 ... L68 ;        <- loop head
    ( _ws.fv[0] <op> _ws.fver <op> _ws.fs[0] <op> JS_WhaleObjShape(ctx,2) ) {
        _ws.fp[0] = JS_WhaleObjFieldPtr(ctx, 2, 682, &_ws.fs[0]);
        _ws.fv[0] = _ws.fver;
        ...
    }
    _ws.fp[0]->tag = 8;
    _ws.fp[0]->u.float64 = _ws.v[2];

Steady state, per field access:

    3 loads  - _ws.fv[fc], _ws.fver, _ws.fs[fc]   (the shape check; JS_WhaleObjShape
                                                   sits on the right of || and does
                                                   not run on a hit)
    2 loads  - _ws.fp[fc], twice, for tag and for u.float64
    2 stores - tag, u.float64

So ~7 memory operations per field, 4 fields, ~28 per iteration. At 156ms / 4M
iterations that is ~130 cycles, or ~4.6 cycles per memory operation - which is what
memory operations cost. The field-access sequence is the whole cost; nothing else in
the loop is close.

After unboxing, per field: 1 load (`v[i].depth`), no shape check, no tag branch, no
pointer chase. 7 -> 1. Order of magnitude 3-4x, i.e. objfields 156ms -> 50-70ms.

## Order of work

1. ~~Measure first.~~ Done, above. The direction is worth doing.

### Which cases this actually helps - checked before writing code

Counting array element stores across the perftest cases:

    array            stores=1
    list_reconcile   stores=1
    nested2d         stores=2
    sieve            stores=2
    objfields        0
    vnode_build      0

**objfields and vnode_build do not store into arrays at all.** They build one object
into a local (`var o = make()`) and update its fields. So unboxing array *elements*
helps array, list_reconcile, nested2d and sieve - and does nothing for the two cases
this line of work has been chasing.

What those two need is the same idea applied to a **local object variable**:
`var o = { tag: 0, ... }` becoming `struct { double tag, key, flags, depth; } o;` with
the shape check and the pointer chase gone. No array involved. The observation
question is identical - did this object ever hold more than one shape, did it escape -
and the measurement above (7 memory operations per field access at 4.6 cycles each) is
what says it is worth 3-4x.

Two targets, one mechanism, one give-up rule:

| target | cases | what unboxes |
| --- | --- | --- |
| local object variable | objfields, vnode_build | the object |
| array elements | array, list_reconcile, nested2d, sieve | the element |

Start with the local object, because that is where the measured cost is.

2. **Not a narrow scene.** An earlier draft of this said "non-escaping, single-shape
   arrays only", which was a slide back into static analysis - and it is not needed.
   The whole point of the observation machinery is that the JIT knows what actually
   happened during the interpreted calls, so it specialises on that instead of proving
   anything:

   | observed during warm-up | generated |
   | --- | --- |
   | one shape for the elements | unboxed struct backing store |
   | two to four shapes | one fast path per shape |
   | more than that, or the array escaped | the general path, as now |

   Escaping is observable too - the array being stored into another object, or handed
   to a call the JIT cannot see through - so it does not have to be proved either. A
   table that grows past its capacity is abandoned, exactly as `WHALE_PROFILE_SHAPES`
   already does for property sites.
3. Generalise only after the above runs on real cases.

## What the observation table needs to grow into

Today it records, per property access site, the shapes seen. The same record wants
keeping per *array element store* - which shape was written. One table, one bound, one
rule for giving up; just a second kind of entry in it.

## What this is not

It is not a JSValue representation change. `JSValue` stays exactly as it is; an array
gets a struct backing store only while its observed behaviour supports it, and it
converts back the moment the observation stops holding, exactly as an unboxed number
does today.
