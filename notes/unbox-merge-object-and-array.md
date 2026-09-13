# Both unboxings converge: one layout, one map pair

Raised by the user, and it is the right frame for the rest of this work.

Object unboxing and array unboxing look like separate features because they start from
different observations - a single object watched through its field accesses, versus an
array watched through the shapes of what gets stored into it. **But what they produce
is the same thing:**

    a flat, typed block of memory, plus maps from a key to an offset into it

For an object, the key is an atom and each field is one slot. For an array, the key is
an integer index and each element occupies a fixed-width record. The generated accesses
are then the same shape in both cases: `block[base + k]`.

So they should be one mechanism with two entry conditions, not two mechanisms:

    observation:            property sites          element stores
                            |                        |
    candidate test:         function-level, mono    array is mono-shaped
                            |                        |
    layout:                 union of field atoms    element record
                            \                       /
    emission:                one flat block + maps + offset accesses

The user's phrasing - "double map" - is about what the block needs to stay correct:
an atom-to-offset map for dynamic property access, and an index-to-offset map for
computed array access. Both exist for the same reason: the block is the single
authoritative copy, and a computed access must reach that copy rather than a second one
maintained beside it.

## Why it is worth merging

- One layout emitter, one offset-access emitter, one set of release paths.
- The GC and reference-count obligations (notes/unbox-gc-and-dynamic-access.md) are
  identical for both, so they get solved once. Getting them right twice is how one of
  them ends up subtly wrong.
- The array side is what makes it general: an array of structs is the case the user
  wants next, and it is the same block with a stride.

## And why it must not be done by rewriting

The object path is currently working code with its own tests passing. The merge has to
be an extension of the emitters rather than a replacement, and the existing behaviour
(what `_ws.fv/fs/fp` does today) has to keep working for anything that is not a
candidate. Anything that changes behaviour for non-candidates is out of bounds.

Order, given all that:
1. object unboxing end to end on objfields, with the memory-safety rules honoured
2. then lift the layout and access emitters so the array case can use them
3. the array side, same rules

Step 1 is the one to finish now.
