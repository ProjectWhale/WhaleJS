# Unboxing a local object: the concrete change

Where the code goes, following notes/unbox-implementation-plan.md and the reservation
rule in notes/unbox-array-elements.md. This is the step that matters for objfields
(note that unboxing array *elements* does nothing for it - it stores into no array).

## What is there now

An object the generator knows about lives in an object slot:

    #define WHALE_OBJ_T(slot)   ((uint8_t)(WHALE_T_OBJ | (((slot) + 1) << 4)))
    #define WHALE_OBJ_SLOT(t)   ((int)((t) >> 4) - 1)

allocated with `whale_objs++` (whale_jit.c:1219 for OP_object), carried in `cur[]`, and
at run time it is a `JSValue` in the frame's object-slot area. A field access goes
through `_ws.fv[fc]` / `_ws.fs[fc]` / `_ws.fp[fc]`, which is the 7-memory-operation
sequence the measurement is about.

## The change

**A new slot kind.** Alongside `WHALE_T_OBJ` add `WHALE_T_STRUCT`, meaning: this slot
is an unboxed C struct in the generated frame, not a JSValue. Its type byte carries the
slot number exactly as `WHALE_OBJ_T` does, so `cur[]` needs no new machinery.

**A struct declaration per unboxed slot.** The generator already emits the frame
(`_ws`) declarations. For each unboxed slot it emits one struct type with a `double`
(or `int64`) per field in the union of the observed shapes, plus a `uint8_t` presence
flag per field that was not in all of them - the reservation rule. Field order comes
from the shape's property list, which is fixed for a given shape, so the layout is
fixed once the observation is.

**OP_object for a candidate slot** emits the struct initialisation instead of
`JS_WhaleObjNew`, and types the slot `WHALE_T_STRUCT_T(slot)`.

**OP_get_field / OP_put_field on a `WHALE_T_STRUCT` operand** emit
`arr_member = value` / `value = arr_member` - one load or store, no `fv`/`fs`/`fp`, no
`tag` branch, no shape check. A field that is not in every observed shape reads through
its presence flag.

**Materialisation, when it must become a JSValue again.** Anything that escapes - the
object going into another object, or into a call the generator cannot see through - has
to be boxed back. That is the same boundary the unboxed number already has, and the
same rule: box at the edge, keep it unboxed inside.

## The candidate test

A slot is a candidate when, for the last `WHALE_PROFILE_CALLS` interpreted calls:

- `JS_WhaleProfShapes` reports the observed shapes for every field access on it, and
  the union of those shapes is within a size bound (a few dozen bytes per object; the
  user's point is that reserving is cheap, so this can be generous)
- nothing suggests it escaped

Both are observations, not proofs, and both already have a home in the profile table.

## First thing to check when implementing

Whether `fold_obj_field_resolve` / `fold_obj_field_load` can be switched over at all,
because the unboxed path skips the `fc` cache pair entirely - the call sites of those
two functions and of `JS_WhaleObjFieldPtr` are the places the new branch has to sit,
and there are only a handful of them.
