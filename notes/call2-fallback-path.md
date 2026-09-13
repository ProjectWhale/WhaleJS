# OP_call2 / call_method: why the fallback is not a one-liner here

`call2` is the give-up point for the two slowest still-uncompiled cases,
immutable_update (572ms) and vnode_build (385ms), so it is the highest-value next
step. The reason it is not the same shape as the four instructions already converted:

## The structure

    case OP_call0..3:
        if (func_slot < 0) goto out;
        if (cur[func_slot] == WHALE_T_FN && !whale_inline_nested) {
            ... inlining path ...
            if (one argument is neither a number nor a slot)
                goto out;                     <- A
            rtype = whale_inline_body(...);
            if (rtype < 0)
                goto out;                     <- B
            break;
        }
        if (cur[func_slot] != WHALE_T_SELF) {
            ... general path, JS_WhaleCallValue at the end ...
        } else {
            ... self-recursion path; more goto out ...
        }

The obvious fix - let A and B fall through to the general path instead of giving up -
does not work as written. The general path is guarded by
`cur[func_slot] != WHALE_T_SELF`, and the callee here is `WHALE_T_FN`: falling out of
the inlining block lands in the `else` branch, not the general one. So the fallback
needs `func_slot` to be re-typed into something the general path accepts before the
jump, or the general path needs to be reachable from the `WHALE_T_FN` case directly.

## What makes this tractable

`JS_WhaleCallValue(ctx, func_slot, argc, arg_base, ret_slot)` already does the whole
job - it exists, it is registered with TinyCC, and its arguments are already parked
values (`slots + arg_base`). The argument-parking code just above the inlining block
already handles `WHALE_T_NUM`, `WHALE_T_VAL` and `WHALE_T_OBJ`, so opaque arguments
are not the problem. The missing piece is purely the control flow: reaching that call
when inlining does not apply.

## Suggested shape

Move the inline attempt out of the way rather than making it the only route:

    if (cur[func_slot] == WHALE_T_FN && !whale_inline_nested) {
        ... try to inline; on success `break` ...
        /* on failure, fall out with no goto, and re-type the slot so the
           general path accepts it */
        cur[var_count + func_slot] = WHALE_T_VAL_T_of_the_callee_slot;
    }
    /* then the existing general path is reached for both WHALE_T_VAL and
       WHALE_T_SELF cases */

The re-typing is the part that needs care and a test: the callee already lives in a
slot (whale_slot_fn[func_slot] names it), so the conversion should be to
`WHALE_VAL_T(that slot)`.

## Related, same file, also still open

- `call_method` (arr_methods, string) - needs the receiver as an argument slot.
- `get_length`, `get_array_el`, `put_array_el` - array shapes, none of which have an
  engine helper yet. `JS_WhaleArrPtr` / `JS_WhaleArrSetNum` exist and cover the packed
  case; `get_length` has nothing.
- `push_empty_string` (stringcat) - identical in shape to `OP_null`, which is already
  done. Cheapest remaining item.
- `put_field` with an opaque value - mirror of `OP_define_field`, which is done; needs
  a `JS_WhaleObjStoreValue`.
