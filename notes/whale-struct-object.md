# The C-struct object: the architecture, stated once and precisely

Written because the same misunderstanding was made and corrected several times in a row.
Read this whole file before touching the code. Everything here is what the user specified;
where a correction was needed, the wrong reading is recorded too, marked **WRONG**, so it
is not arrived at again.

## The one sentence

A compiled function's objects are a **new kind of object** whose property values live in a
flat C array that native code addresses by a compile-time offset; it is *not* a JSObject
with something bolted on.

## What it is not (each of these was tried and is wrong)

- **WRONG: an extension of `JS_CLASS_OBJECT`.** It is a separate class with a separate tag.
  The two share nothing and neither accommodates the other. Trying to hang a native form
  off an ordinary object is what led to "materialise at the boundary", "re-lay the layout on
  each read", and two rounds of broken code.
- **WRONG: a `JSObject` field, or a second pointer inside it.** Nothing is added to
  `JSObject`.
- **WRONG: the shape is the offset map.** The shape serves the *dynamic* properties. The
  offset map is its own map object.
- **WRONG: a `set` flag / a `WhaleSlot{double, uint32}`.** There is no flag. The distinction
  it was meant to carry is in the **value**, and a separate field would break up the flat
  array.
- **WRONG: values in two places (`data` and also `prop`).** There is one copy. The two name
  sets do not overlap, so there is nothing to keep in step.
- **WRONG: materialising, boxing or re-deriving anything at a function boundary.** There is
  no boundary cost. The struct travels with the object.
- **WRONG: frame-side columns (`_ws.sv[]`) alongside the object.** That is the old scheme,
  and it is what made objects stop existing (`OP_object` took the unboxed path and `break`
  -ed past `JS_NewObjectClass`, so the interpreter never saw the object at all). It is to be
  removed, not extended.
- **WRONG: "`in` consults the offset map, not the value."** I wrote that, and it
  contradicts what the user said from the start. See the next section.

## `in`, and undefined: it is the VALUE that decides

Every column is a `JSValue`. Two of them look identical through a plain read and must be
told apart by `in`:

| column | meaning | plain read | `in` / `hasOwnProperty` |
| --- | --- | --- | --- |
| `tag == undefined`, **value == 0** | the property **was set** to `undefined` | `undefined` | **true** |
| `tag == undefined`, **value != 0** | **reserved, never set** | `undefined` | **false** |
| any other tag | an ordinary value | that value | **true** |

So the rule is literal: `in` looks at the column's `JSValue` - `tag == undefined` means it
reads as `undefined`, and a **`value` of 0 is a real value** (the property is there) while a
**nonzero `value` is the reserved marker** (the property was never set). The offset map is
only what finds *which* column a name occupies; the answer to `in` comes from the value.

A shape-union layout reserves columns for properties a given object may never set - by
design, because one extra column costs very little and keeping near-identical shapes on one
layout is a large win. The payload encoding is what makes that free: a reserved column is
indistinguishable from a real `undefined` through every ordinary read, and only `in` cares.

**There is no `set` flag.** The payload carries it.

## The three parts

    { offsetMap, dynamicMap, data }

| part | what it holds | where |
| --- | --- | --- |
| **offset map** | name -> column. **Only the mapping.** No values. | a map object |
| **dynamic map** | **only** the names the offset map does not cover, as properties of an ordinary object - ordinary shape/prop storage | a plain object |
| **data** | the values for the offset map's names, flat: one `JSValue` per column, 16 bytes each, in column order | one allocated array |

"Struct" is a way of thinking about it, **not a layout anyone declares**. There is no struct
definition for the generated code to see and no column definition it could depend on. In
generated code a property access is: the `data` pointer plus a compile-time offset - two
constants and a load.

## Lookup, on both sides

- **Native code** knows its column at compile time: one load from `data`. It only falls to
  the dynamic path for a name it was not built for.
- **JS side (interpreter, getters, anything the JIT did not compile)**: look the name up in
  the offset map; hit -> use `data[col]` (applying the `in` rule above where it matters);
  miss -> the dynamic map.

Because the two name sets are disjoint, **no property is stored twice**.

## The maps

`offsetMap` and `dynamicMap` are both ordinary objects, held as `JSValue`s like any other
value, so they are freed and traced the ordinary way. `dynamicMap`'s storage is the engine's
usual shape/prop machinery. `offsetMap` maps a name to a column index (byte offset =
column * 16 gives the address into `data`); its values are column numbers, never property
values.

## What this replaces

The current generator keeps per-function frame columns (`_ws.sv[]`, `_ws.sp[]`) and unboxes
locals into them, then has to **materialise** an object whenever it has to become a real
JSValue (call argument, return value) and **re-lay-out** it when it arrives in another
compilation. All of that exists only to work around objects not being real. With the
C-struct object:

- `OP_object` builds the object (it exists from that moment, and the interpreter sees it)
- field reads/writes are `data + offset` in the generated code
- a call argument or return value needs **no conversion** - the object already is one
- `OP_get_arg` needs **no laying out** - the argument's own struct is already there

Everything in that list is to be **deleted**, not adapted: the frame columns, the
materialise-on-argument code, the layout-on-`get_arg` code, the struct/`VAL` type threading
through `put_loc`/`get_loc`, and the `WHALE_VALUE_OBJ` value-position fence (its stated
reason was a codegen bug that does not exist under this design).

## Engineering notes

- The class's prototype is `Object.prototype` and its class_name is `Object`: in JS it must
  look ordinary and nothing may look unusual about it.
- GC: the values in `data` are `JSValue`s owned by the object; `finalizer` frees each, then
  `data`, then `offset`, then `dynamic`, then itself. `nslots` is the count.
- Measured gap this is meant to close: `objfields` (object made in the same function)
  64-80ms vs a cross-function call holding the same object at 159ms, with the cross-function
  case showing `sv[] writes: 0` and `ObjFieldPtr: 13` - i.e. it fell back to the map path.
  Stock qjs on the same case is ~584ms; **always run the qjs control before concluding
  anything about whether code compiled.**

## Verification discipline (learned the hard way)

1. Run `node` **and** stock `qjs` on the same case **before** writing any conclusion. A time
   close to qjs means it did not compile.
2. Check `check=` against node; a wrong answer is worse than a slow one.
3. `bail=N` from `WHALE_JIT_DUMP` names the line that refused - use it instead of guessing
   from `pos`/`op`, which reports the jump target, not the reason.
4. Do not report a design conclusion without the numbers that support it.
5. **Do not substitute your own reasoning for what the user said.** The `in` rule was stated
   plainly - `in` reads the value - and then overridden by an inference of mine. That is
   what produced this correction. If a detail is not in this file, ask; do not derive it.
