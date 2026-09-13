# Mixed compilation: the fallback is "run this ONE instruction", never "give up"

The structural correction. Everything in this file is what `goto out` should have been.

## What is wrong now

The generator's fallback for anything it cannot compile is `goto out`, which abandons
the **whole function** and sends it back to the interpreter. That makes every
incremental change all-or-nothing: adding one new type (WHALE_T_STRUCT) or one new case
either covers everything in the function or the function is not compiled at all. It is
why a struct-typed object made `object` go from 8ms to 416ms - the first access it
could not handle threw away the whole compilation.

The design is: **an instruction that cannot be compiled becomes a call into the VM that
executes that one instruction**, and the code around it stays native. Then a new type
or opcode is always a local, bounded loss - one instruction goes through the
interpreter - and never a global one.

## Why it is unusually cheap here

Because of how quickjs lays out a running function. The objects the generated code
already uses are not private to it: the object-slot area the JIT parks things in sits
immediately after the JS operand stack, on the frame the engine itself walks. So the
native code and the interpreter are looking at **the same memory**, not two copies that
need keeping in step. Executing one instruction from native code therefore means
handing the VM a pc and letting it run, with the frame already in a state it
understands.

## What has to be true for one instruction to be executable this way

1. **The pc must be the bytecode offset of the instruction.** The generator is walking
   the bytecode and knows it.
2. **The operand stack must agree.** The generated code models it in `_ws.v[]` and by
   the compiler's own type tracking; the VM expects the live stack. Where the two
   differ, the difference has to be settled at the boundary - either the values are in
   the frame's stack area already, or they are written there before the call.
3. **Locals must agree.** The generated code has its own names for locals; the VM reads
   `var_buf`. Same question, same boundary.
4. **The result must come back.** Whatever the instruction pushed has to be visible to
   the code that follows, in the representation the generator tracks.

Points 2 and 3 are where the work is, and they are worth settling before writing
emission code, because getting them wrong produces exactly the class of bug this whole
work has been fighting: a wrong answer that looks like a slow path.

## Order

1. **Settle the boundary** (2 and 3 above) - what the generated function's frame looks
   like to the engine at the moment a fallback is taken.
2. **One engine entry point**: run the single instruction at a given offset.
3. **Convert the fallbacks one at a time**, starting with the type-mismatch cases
   (`!WHALE_IS_SLOT(ta)` in the field accesses), which are the ones blocking the struct
   work.
4. **Only then** the emission halves for the struct, which by then are a pure
   optimisation rather than a correctness requirement.

Note that step 3 also lets most of the hand-written special cases added during this
work be deleted. `OP_null`, `OP_push_atom_value`, the binop fallback and
`JS_WhaleObjStoreValue` were each written to rescue one instruction from `goto out`;
with a general fallback they are just the general case.
