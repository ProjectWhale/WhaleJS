# A real bug behind vnode_build: OP_get_var_ref marks a function as a number

## Symptom

vnode_build still does not compile after the call2 fallback was added, and the
diagnostic says why:

    [call] pos=34 argc=2 tc=1 slot=0
    [call] arg ... rejected

`pos=34` is `createElement(i % 5, { id: i, cls: "c" + (i % 3) })` in

    function createElement(type, props) { return { type: type, props: props, key: null }; }
    function render(n) {
        for (var i = 0; i < n; i++) {
            var v = createElement(i % 5, { id: i, cls: "c" + (i % 3) });
            out += v.type + v.props.id;
        }
    }

`createElement` is an enclosing function, so reading it inside `render` is an
`OP_get_var_ref`. And `tc = WHALE_T_NUM` - the callee was tracked as a number.

## Why

whale_jit.c, `case OP_get_var_ref0..3 / OP_get_var_ref`:

    if (!whale_var_ref_feeds_call(pc, len, pos, sz)) {
        ... JSON: JS_WhaleVarRefGetNum(ctx, var_refs, idx, &v[nm]) ...
        cur[var_count + sp] = WHALE_T_NUM;    /* read it as a number */
        sp++;
        break;
    }
    if (!whale_assume_self) { ... park it as a value ... }

`whale_var_ref_feeds_call(pc, len, pos, sz)` is a short lookahead: it walks forward
looking for pushes that lead into a call opcode, and stops after a few of them
(whale_jit.c:419 onwards). For `createElement(i % 5, {...})` the argument in between
involves a modulo and an object literal, so the lookahead does not reach the call, the
function returns 0, and the slot is typed WHALE_T_NUM.

The value really is a function. Downstream, the general call path requires
`WHALE_IS_SLOT(tc)` (whale_jit.c:3262), which a number is not, so the call gives up.

## The fix, and why it was not applied here

The right answer is the `!whale_assume_self` branch a few lines below: park the value
in an object slot (`whale_objs++`) and type it `WHALE_VAL_T(slot)`, which both a
numeric read and a call target can then handle. What makes this delicate is that the
numeric branch is also the fast path for a genuine captured counter - a closure
incrementing a number - and switching it unconditionally would slow every such read
down to a slot round trip.

Two candidate shapes, neither yet tried:

1. **Widen the lookahead.** `whale_var_ref_feeds_call` currently stops early; letting
   it scan to the end of the basic block would cover this call. Cheap, but it is a
   heuristic and will still miss something eventually.
2. **Ask whether the value is ever a call target, not whether it feeds one.** A
   pre-scan pass over the function can record, per var_ref slot, whether any
   `OP_call*` uses that slot as `sp - 1 - argc`. That is exact, and the pre-scan
   already exists (it walks every opcode to collect put/get field atoms, back edges
   and var_ref_used). `var_ref_used[]` is essentially this flag already - it is set
   when a var_ref feeds a call, which is the same weak test.

Option 2 is the one to do: it removes the heuristic rather than extending it.

## Effect of everything else

immutable_update does reach the general path now (tc=56, a slot) and still reports one
give-up; its time did not move (583 -> 641ms across the change, both within this
machine's spread). vnode_build 356 -> 360ms. So the fallback work so far has not
produced a speedup on any case - the two slow ones both still stop before the end of
the function, and the cases it did fix were already fast.

That is the honest state: the mechanism is in place and correct, the remaining
blockers are specific and identified, and none of them is the "abandon the function"
policy any more except where the operand typing is wrong - which is this bug.
