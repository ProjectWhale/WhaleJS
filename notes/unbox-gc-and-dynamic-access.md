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

## What actually constrains the first version

Not "all fields numeric". The constraints are:

1. Every field access on the slot was observed monomorphic (already the candidate
   test), so the layout is known.
2. The offset map exists before any dynamic access can reach the object, or the object
   is not unboxed.
3. Whatever is attached to the object is released with it.

Order: objfields first (all-numeric, no dynamic access - the simplest case that proves
the mechanism), then the map, then non-numeric fields.
