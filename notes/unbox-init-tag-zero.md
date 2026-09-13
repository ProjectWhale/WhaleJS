# Unboxed struct fields: undefined is all-zero, so "unset" needs its own 128-bit pattern

Corrected by the user twice; the first version here had the tags the wrong way round.

## The fact that settles it

**A normal `undefined` JSValue is zero** - tag 0 and value 0 together. That is what the
engine uses, so an unboxed field cannot use "tag 0" to mean "nothing was ever stored
here": tag 0 is exactly what a field holds after being assigned `undefined`, which is
a real, present value.

## The layout

Each field is **128 bits**: a tag word and a value word, written together.

    assigned undefined   tag = JS_TAG_UNDEFINED (3)   value = 0
    never assigned       tag = JS_TAG_UNDEFINED (3)   value != 0    (unset)
    assigned a number    tag = JS_TAG_FLOAT64 (8)     value = <bits>

The actual tag values, checked rather than assumed (quickjs.h:181-200):

    JS_TAG_INT = 0, JS_TAG_BOOL = 1, JS_TAG_NULL = 2, JS_TAG_UNDEFINED = 3,
    JS_TAG_UNINITIALIZED = 4, ..., JS_TAG_FLOAT64 = 8

Two things fall out of that and both matter:

* undefined is tag **3**, not 0 - so nothing here may be written from memory.
* **a memset is not just imperfect, it is wrong**: zero bytes mean tag 0, which is
  JS_TAG_INT, i.e. the integer 0. A block of zeros is a block of zeroes, not a
  block of undefineds.

The two words are set as one 128-bit store, which is what keeps them consistent - there
is no window where the tag says one thing and the value another.

Note that the tag is the SAME in the first two rows - both are `undefined`. What
separates them is the value word: zero for a real JSValue, non-zero for the unset
pattern. Membership is therefore:

    (tag == undefined && value != 0)  ->  NOT a member
    everything else                    ->  a member

So there is **no memset shortcut**: a memset writes the same byte everywhere and cannot
produce "tag non-zero, value zero". Initialisation is one 128-bit store per field. That
is the price of keeping `undefined` and "unset" apart, and it is a small price - the
store is a single instruction and only happens at creation.

(The earlier note in this file claimed a single memset would do it. That only works if
"unset" is all-zero, which would make it indistinguishable from `undefined`.)

## Why they must be kept apart: the `in` operator

`'x' in obj` is true exactly when the property was set, **including when it was set to
`undefined`**. With tag 0 alone the two states collide and an explicitly-cleared field
would report as missing. With the pattern above, membership is "tag != 0", and both the
present-undefined and absent cases still read back as `undefined`.

Once fields can hold references the same two words carry the reference-count answer: a
field assigned `undefined` holds a value (nothing to free, but it is a value that was
written); a field never assigned holds nothing at all. The tag word is what says which.


## Follow-up: tag 3 is what MAKES the memset work (user)

The tag being 3 rather than 0 is not a problem to work around - it is what allows
the one-memset initialisation after all.

The marker for "not a member" is an IDENTICAL value in both words: tag and value
both equal to the undefined tag (3). A memset that writes that byte pattern across
the data area therefore produces, for every field, a word pair that is exactly the
unset pattern - no per-field store at all.

    initialisation:   memset(data_area, 3, size)   /* or whatever fills both words */
    membership test:  tag == value == JS_TAG_UNDEFINED  ->  NOT a member
    real undefined:   tag == JS_TAG_UNDEFINED, value == 0  ->  a member
    a number:         tag == JS_TAG_FLOAT64,    value == bits

So the finished shape is: memset the data area, plus the two maps (atom -> offset
for property access, index -> offset for array access). That is the layout the user
described from the start, and it needs no per-field initialisation instruction.

Note the exact pattern must be taken from the engine rather than assumed - the word
boundaries and the tag width are things to read, not to remember. This section is
about the structure, which is settled: both words carry the marker, so one memset
is enough.