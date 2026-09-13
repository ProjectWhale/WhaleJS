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
2. **Nothing else.** The stack, the arguments and the locals are all on the frame,
   in memory, and the generated code can reach all of it. There is no separate
   representation to convert and nothing to write back: hand the engine the frame
   pointer and let it run the instruction. (Corrected by the user - the earlier
   version of this note proposed marshalling between two representations, which is
   not how it works here.)

That is what makes the fallback cheap enough to be the default: the interpreter and
the native code are looking at one set of values, so crossing between them is a
pointer, not a copy.

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
