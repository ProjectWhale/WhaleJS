# Unboxing: the implementation plan, with the exact insertion points

Follow-on from notes/unbox-array-elements.md. The measurement says the direction is
worth 3-4x on objfields (7 memory operations per field access, down to 1). This is
where the code goes.

## 1. Observation: record the shape of every value stored into an array

The interpreter's array store is

    CASE(OP_put_array_el):           /* quickjs.c:21559 */
        val = sp[-1];
        if (likely(JS_VALUE_GET_TAG(sp[-2]) == JS_TAG_INT)) {
            idx = ...;
            if (likely(JS_VALUE_GET_TAG(sp[-3]) == JS_TAG_OBJECT)) {
                p = JS_VALUE_GET_OBJ(sp[-3]);
                if (likely(p->class_id == JS_CLASS_ARRAY && idx < p->u.array.count)) {
                    set_value(ctx, &p->u.array.u.values[idx], val);   /* <-- here */
                    ...

Right before that `set_value`, and gated on `b->whale_prof_on` exactly as the
property recorder already is:

    if (unlikely(b->whale_prof_on) && JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT)
        whale_prof_record_arr(b, p, JS_VALUE_GET_OBJ(val)->shape);

Three copies of this case exist (the interpreter has several main loops), same as the
property recorder - all three need it or the observation is partial.

`whale_prof_record_arr` is the array twin of `whale_prof_record`: same direct-mapped
entry, but keyed on the *array* (its address, or better the bytecode offset of the
`put_array_el` if the array is local - the address moves with GC), and storing the
shapes of the elements written rather than of the property reads. Same bound
(WHALE_PROFILE_SHAPES) and the same rule: past the bound, the entry is disabled and
the general path is used.

Keying question to settle first: the array may be a local (stable address) or may come
from anywhere. For the local case the bytecode offset identifies the site as well as
it does for property reads. For a non-local array, keying on the object address is
unsound across GC, so the honest options are (a) only record for arrays the generator
can identify statically, or (b) give each observed array an id in the table. (a) is
simpler and probably covers vnode_build.

## 2. Generation: a struct backing store

The JIT currently keeps arrays as opaque object slots and reaches them through
`JS_WhaleArrPtr` / `JS_WhaleArrSetNum` (packed-array specialisation, already present).
The unboxed form is a different thing: a C struct per element shape, allocated in the
generated function, indexed directly.

- The generator needs the element shape's property list (atoms, in a fixed order) at
  compile time. That is available: the observed shape is a JSShape, whose property
  list gives the atoms; the generator has `info->cpool`-style access for atoms.
- Field access becomes `arr[i].field_k` where k is the index in that list.
- Store becomes `arr[i].field_k = <double>`.
- Box on the way out: the value must become a JSValue again if it escapes, so there is
  a materialisation step at the boundary - the same shape as the existing number
  unbox/box boundary.

## 3. What to check before writing any of it

- Which arrays the generator can identify as local and single-shaped, in the real
  cases. Run a count over the perftest cases: how many `put_array_el` sites have a
  local array operand whose elements were all one shape? If that count is near zero in
  objfields/vnode_build the whole direction needs rethinking, and that is a cheap
  script, not a compiler change.
- Whether the element shapes in those cases have a stable property order. A struct
  needs a fixed layout; a shape with properties added in different orders gives
  different shapes anyway, so one observed shape means one order - worth confirming
  rather than assuming.

## State of the work it follows on from

Committed up to `ed5469c`. The general fallback is in place for null, push_atom_value,
binary operators, define_field and put_field. Measured gains so far come entirely from
`3e7a5f5` (the lookahead bug): vnode_build -8%, immutable_update -6%, objfields -8%.
24 cases at 0 errors / 0 mismatches.
