# Unboxing: two constraints raised by the user, and what they force

## 1. GC: nothing may leak

The unboxed struct itself is safe by construction as long as its fields are plain
doubles: `_sv[]` holds no JSValue, so there is nothing for the collector to see and
nothing to free. That is not an accident, it is the reason to start there.

It stops being trivially safe the moment a field can hold a reference - a string, a
nested object, anything with a tag. Then the frame holds live JSValues that the
collector does not scan (the generated function is a plain C function with no
JSStackFrame), so they would be collected underneath it. That is exactly the problem
`whale_needs_obj` and the object-slot area already solve for objects: the slots sit
immediately after the JS operand stack, where the engine walks them.

So **the first version only unboxes objects whose observed fields are all numeric**.
objfields is exactly that - tag, key, flags, depth are all numbers - and it is the case
the measurement is about. A shape with a string field is not a candidate yet; it needs
either the object-slot machinery for the non-numeric fields or a materialise/release
pair, and that is a separate step with its own correctness argument.

Worth stating plainly because it is the kind of thing that shows up much later as a
crash under GC pressure and looks unrelated.

## 2. Dynamic property access: `obj[str]` needs the offsets somewhere

If a property is read or written by a computed key, the access site has no atom at
compile time, so an unboxed struct has nothing to resolve at compile time either. The
user's point: such an object may need a map, carried alongside it, from atom to slot
offset, so a dynamic access can still find its field.

For the first version the answer is simpler: **an object with a dynamic access is not
a candidate.** The candidate test already requires every field access on the slot to
come back monomorphic from `JS_WhaleProfShapes`, and a computed-key access has no atom
to record a shape for in the first place - it will not appear in that set, and its
presence has to disqualify the slot rather than be quietly ignored. That is a check to
add to the candidate test, not a redesign.

The map is the general answer and it is worth keeping in mind, but it is also what
makes an unboxed struct stop being "just a C struct" - the moment there is a side
table, the access is a lookup again and most of the 7-to-1 win is gone. Better as a
later refinement for objects that are mostly accessed by atom.

## What this means for the order

1. All-numeric fields, no dynamic access. objfields, and that is it - the case with a
   3-4x measurement behind it.
2. Then look at non-numeric fields, which is where the GC question becomes real.
3. The offset map only if dynamic accesses turn out to be common enough to matter.
