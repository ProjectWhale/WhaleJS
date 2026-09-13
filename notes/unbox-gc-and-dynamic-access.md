# Unboxing: the object stays a JS object, and the map is the fallback

Corrected after the user clarified both points. My earlier reading had both wrong.

## 1. The unboxed object is still an ordinary JS object

I wrote that an unboxed struct is "safe as long as its fields are plain doubles" and
that non-numeric fields would need the object-slot machinery. That is not the design.
**The object keeps its identity as a normal JSObject the whole time, so GC keeps
managing it and frees it normally.** Unboxing changes where the field *values* live and
how they are reached - offsets instead of a property lookup - not what the object is.

What that means for correctness:

- the object is allocated and freed as any other, so nothing is freed underneath the
  generated code
- anything the object itself references is reachable in the ordinary way
- the only new obligation is that whatever we attach to the object has to be released
  with it - the offset map below, if it exists. That is the leak to watch for, and it
  is a much smaller surface than "the collector cannot see this".

## 2. The offset map is the fallback for dynamic access, not a disqualifier

Also wrong in my first draft: I proposed refusing to unbox any object with a computed
key access. The user's point is sharper, and it is about not having **two copies of one
property**.

If a property is unboxed into a slot and a dynamic access (`obj[expr]`) could still
write it through the ordinary property table, then the same property exists twice - in
the struct and in the object's property storage - maintained independently. Whichever
one an access happens to read is then a coin flip. That is a correctness bug, not a
performance question.

The fix is a map carried by the unboxed object, from atom to slot offset, used **only**
by dynamic accesses:

- a static access (`obj.tag`) goes through the offset, as before
- a dynamic access (`obj[k]`) resolves through the map to the same slot
- so there is exactly one copy of every property, and both routes reach it

Cost: creating the map, and a lookup on the dynamic path. **The hot path is unchanged** -
it is still a slot offset - so the map costs nothing where it matters. It is the
fallback, not the mechanism.

And the behaviour stays that of an ordinary object: **fields start out as
`undefined`**, so an unboxed object answers the same as a normal one for a property
that was never assigned. The presence flags already in the layout (`_sp`) are what make
that expressible - absent means `undefined`, not "garbage".

## 3. Reference counts: the correctness point

Raised by the user, and it is the one that turns a performance change into a possible
crash. quickjs is reference counted, and the helpers already follow a clear convention:

    JS_WhaleObjSlot(k, v):   JS_FreeValue(ctx, slots[k]);  slots[k] = v;
                             /* release what was there, take over the caller's ref */
    JS_WhaleObjGet(k):       return js_dup(...);           /* hand back an owned ref */
    JS_WhaleObjStoreValue:   JS_FreeValue(ctx, *dst); *dst = v;

The existing code is correct because the slot owns exactly one reference. Once a
struct holds a JSValue field, the same obligation applies to a place the engine does
not know about, and it has to be met at every point:

- **storing** into a field releases the old value before taking over the new one, the
  same shape as above
- **releasing the struct** releases every JSValue field it holds, exactly once
- a field that was never assigned reads as `undefined` (see `_sp`), which is a
  non-owning constant and must not be freed

Getting this wrong does not show up as a wrong answer, it shows up much later as a
use-after-free or a leak, which is why it belongs in the design and not in a later
cleanup pass.

Numeric fields sidestep it entirely - a double carries no reference - which is another
reason the first version starts with objfields.

## What actually constrains the first version

The constraints are:

1. Every field access on the slot was observed monomorphic (already the candidate
   test), so the layout is known.
2. The offset map exists before any dynamic access can reach the object, or the object
   is not unboxed.
3. Whatever is attached to the object is released with it.
4. Every JSValue held in a struct field is owned and released exactly once - see above.

Order: objfields first (all-numeric, no dynamic access, no refcount obligation - the
simplest case that proves the mechanism), then the map, then non-numeric fields, which
is where point 4 starts to bite.
