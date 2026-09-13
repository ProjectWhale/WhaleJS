# Unboxing: how a slot becomes a candidate, and why OP_object needs to know early

Where the next step got stuck, and the way through. Follows
notes/unbox-local-object-plan.md.

## The structural problem

`OP_object` is where the struct initialisation would be emitted, but at that bytecode
position the generator does not yet know anything about the field accesses that come
later. The candidate test is exactly about those later accesses - "every access on this
slot was monomorphic" - so it cannot be answered at the point where it is needed. It
has to be answered before, from a pre-scan.

## Why the existing pre-scan does not already have it

The pre-scan (the loop that fills `loop_back`, `assign_cnt`, `induction`, `var_ref_used`
and `whale_loc_fn`) walks every opcode but does **not** simulate the value stack. What
it records is keyed on *local variables*, not on stack slots, because that is what its
existing users need (`whale_loc_fn` maps local -> pool entry for a function literal,
built from `fclosure` + `put_locN`).

## The way through: key on the local, not on the stack slot

The objects worth unboxing are the ones built into a local - `var o = make()` or
`var o = { ... }` - which is the shape in objfields and vnode_build. That means the
pre-scan can use exactly the mechanism `whale_loc_fn` already uses:

1. `object` (+ `define_field`*) followed by `put_locN` marks local N as an object
   candidate, the same way `fclosure` + `put_locN` marks it as a function.
2. `get_locN` followed by a field access records that access site's offset against
   local N.
3. The candidate test then runs over the offsets collected for N: every one of them
   must come back from `JS_WhaleProfShapes` with exactly one shape, and the union from
   `whale_struct_fields` must fit in `WHALE_STRUCT_FIELDS`.

This needs no stack simulation, which is what made the direct approach look expensive.

## What it costs

A per-local array of access offsets, bounded the same way everything else is
(`WHALE_JIT_MAX_SLOTS` locals, a small cap on offsets each). Filled during the pass
that already walks the bytecode, so no extra traversal.

## Where the result is consumed

`put_locN` (or `OP_object` when it can see through to the local, whichever is simpler)
emits the struct initialisation instead of `JS_WhaleObjNew` and types the slot
`WHALE_STRUCT_T(slot)`; `OP_get_field` / `OP_put_field` then see that type and emit
`_sv[base + k]` directly.

## Also worth noting for whoever picks this up

Every helper added in this series needed four things, and forgetting one has cost time
three times now:

1. the engine function,
2. its declaration in whale_jit.h,
3. `tcc_add_symbol` in whale_jit.c,
4. a prototype emitted into the generated C - and only for the ones the generated code
   actually calls.

And separately: a new static function must be defined **above** its first caller. That
one has also cost time three times (`js_strict_eq_slow`, `get_shape_prop`,
`whale_prof_one_shape`).
