# General fallback: where it stands

The direction the user set early on: when an instruction cannot be compiled, dispatch
that ONE instruction into the VM and keep the rest native. `goto out` abandoning the
whole function is too blunt, and it is why whole classes of ordinary code never
reached the JIT.

## What is done

`OP_null` and `OP_push_atom_value` (commits 6294592). Both used to abandon the
function. They now park the value in an object slot as opaque:

    case OP_null:              JS_WhaleObjSlotNull(ctx, os)
    case OP_push_atom_value:   JS_WhaleObjSlotAtom(ctx, atom, os)

followed by `cur[...] = WHALE_VAL_T(os)`. Two narrow engine helpers exist for this, so
the generated C never needs to know about JS_NULL or the atom-to-value rules.

The type byte is what makes this safe: WHALE_T_VAL is a slot, so a downstream
instruction that needs a number looks at it with `whale_type_is_num` and refuses.
That refusal is a `goto out`, so the current behaviour degrades to "abandon", not to
"compute something wrong". Verified: 24 cases, 0 generation errors, 0 mismatches.

## The mechanism to be clear about

Pushing an opaque value only moves the give-up point forward; it does not remove it.
That is expected and it is the right order: each step makes the give-up land on a
later, more specific instruction instead of the first one.

## Remaining give-up points, by how much they block

    add               <- fib, string, vnode_build      (3 cases)
    get_array_el      <- list_reconcile, nested2d
    get_length        <- array, arr_methods
    put_field         <- objfields
    define_field      <- vnode_build
    push_empty_string <- stringcat
    call2             <- immutable_update
    call_method       <- arr_methods
    put_array_el      <- nested2d

## Next step, concretely

**Binary operators with an opaque operand.** `OP_add` and friends test
`whale_type_is_num(ta) && whale_type_is_num(tb)` (whale_jit.c:3241) and give up
otherwise. When one side is a WHALE_T_VAL the correct behaviour is not to give up but
to call the engine and let it do the real JS operation - `JS_Add`, `JS_Sub`, and so on
- storing the result in a fresh opaque slot.

That single change covers `add` (3 cases) and is the pattern every remaining operator
needs:

1. engine helper: `int JS_WhaleBinopVal(JSContext *ctx, int op, JSValue a, JSValue b,
   int out_slot)` - reads both slots, applies `JS_Add`/`JS_Sub`/..., parks the result.
2. generator: in the `OP_add`/`OP_sub`/... case, when either side is a VAL, emit the
   helper and set `cur[...] = WHALE_VAL_T(new_slot)` instead of `goto out`.
3. `define_field` and `put_field` with an opaque value need the same treatment
   (`JS_WhaleObjDefineValue`), which is what unblocks vnode_build.

## Two things the evidence does NOT support

- **The general fallback is not needed to make objfields fast.** objfields is 166ms
  and object is 147ms with the memo defeated; there was never a 27x gap. See
  notes/correction-memo-and-giveup.md.
- **`gen gave up` in the log is not proof a function is uncompiled.** whale_on_call
  compiles two readings (quickjs.c:3878/3887) and the second usually succeeds. Count
  `whale_jit_fn_N -> ...` lines, not `gave up` lines. The five cases that genuinely
  end up uncompiled are vnode_build, arr_methods, nested2d, string, stringcat - and
  of those only vnode_build is slow (338ms).

## Measurement warning, third occurrence

object.js is `for (r...) check = tally(200000)` - the same argument every time, and
`tally` is pure, so the engine's memo answers nineteen of the twenty calls. Any
benchmark whose loop repeats a pure call with fixed arguments measures the memo table.
temp/measure.ps1 (minimum of N runs) does NOT protect against this; the arguments have
to change.
