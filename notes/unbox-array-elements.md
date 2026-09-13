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

## Order of work, as I would do it

1. **Measure first.** Establish how much of objfields' 156ms is the field-access
   sequence and how much is everything else. If the shape/version/pointer loads are
   not the bulk, this direction is worth much less than it looks.
2. Non-escaping, single-shape arrays whose elements are only ever touched through
   integer indices and field accesses - the narrow case that covers vnode_build.
3. Only then consider generalising.

## What this is not

It is not a JSValue representation change. `JSValue` stays exactly as it is; the array
that gets a struct backing store is one that the JIT has proven local and
single-shaped, and it converts back the moment it might escape, exactly as an unboxed
number does today.
