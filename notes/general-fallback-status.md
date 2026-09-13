# General fallback: where it stands

The direction the user set early on: when an instruction cannot be compiled, dispatch
that ONE instruction into the VM and keep the rest native, instead of abandoning the
whole function. `goto out` was abandoning it.

## Done, each verified at 24 cases / 0 generation errors / 0 mismatches

| instruction | handled by |
| --- | --- |
| `OP_null` | `JS_WhaleObjSlotNull(ctx, os)`, slot typed `WHALE_T_VAL` |
| `OP_push_atom_value` | `JS_WhaleObjSlotAtom(ctx, atom, os)`, same |
| binary ops with an opaque operand | `JS_WhaleBinopVal`, dispatching into the engine's existing `js_add_slow` / `js_binary_arith_slow` / `js_binary_logic_slow` / `js_relational_slow` / `js_eq_slow` |
| `OP_define_field` with an opaque value | `JS_WhaleObjDefineValue(ctx, slot, atom, val_slot)` |

The mechanism: the value is parked in an object slot and its stack type is
`WHALE_T_VAL`. Anything downstream that genuinely needs a number still refuses; the
difference is that the refusal now happens at a specific instruction instead of at the
first opaque value, and the fallback keeps moving forward through the function.

## Two traps when adding the next one, both hit here

1. **A generated call needs BOTH a prototype in the emitted C and a `tcc_add_symbol`.**
   The comment above the existing prototypes in whale_jit.c says so, and it was not
   read. Without them TinyCC falls back to an implicit declaration, passes arguments
   wrong, and reports `undefined symbol` - nothing compiles at all.
2. The generator-side helper must be a **macro**: `whale_objs`, `cur` and `var_count`
   are locals of `whale_jit_gen`.

## The give-up list, shrinking

    call2              <- immutable_update, vnode_build
    get_array_el       <- list_reconcile, nested2d
    call_method        <- arr_methods, string
    get_length         <- array, arr_methods
    return             <- fib
    push_empty_string  <- stringcat
    put_array_el       <- nested2d
    put_field          <- objfields

`add` and `define_field` used to be on it and are gone.

## Next, in the order I would try them

1. **`call2` with opaque arguments.** `JS_WhaleCallValue(ctx, func_slot, argc,
   arg_base, ret_slot)` already exists and already parks a live result; what is missing
   is parking opaque argument values before the call. This unblocks immutable_update
   (572ms) and vnode_build (385ms) - the two slowest cases still uncompiled.
2. **`get_length`** (array, arr_methods), then **`get_array_el` / `put_array_el`**
   (list_reconcile, nested2d).
3. **`call_method`** (arr_methods, string).
4. **`push_empty_string`** (stringcat) - the same shape as OP_null.

## Two things the evidence does NOT support

- **The general fallback is not needed to make objfields fast.** objfields is 166ms and
  object is 147ms with the memo defeated; there was never a 27x gap. See
  notes/correction-memo-and-giveup.md.
- **`gen gave up` in the log is not proof a function is uncompiled.** whale_on_call
  compiles two readings (quickjs.c:3878/3887) and the second usually succeeds. Count
  `whale_jit_fn_N -> ...` lines, not `gave up` lines.

## Measurement warning, third occurrence

object.js is `for (r...) check = tally(200000)` - the same argument every time, and
`tally` is pure, so the engine's memo answers nineteen of the twenty calls. Any
benchmark whose loop repeats a pure call with fixed arguments measures the memo table.
temp/measure.ps1 (minimum of N runs) does NOT protect against this; the arguments have
to change.
