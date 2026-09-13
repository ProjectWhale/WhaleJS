# Unboxed struct initialisation: memoise the block and let tag 0 mean "unset"

Raised by the user, and it removes the per-field initialisation cost.

Every property of a fresh unboxed object reads as `undefined` when it was never
assigned, which is what makes the behaviour match an ordinary object. The obvious way
to get that is to write `undefined` into every field on creation - one store per field
per object.

Instead, **separate "never assigned" from "assigned the value undefined"**, and do it
with the tag:

    tag == 0        never assigned      -> reads as undefined, `in` is FALSE
    tag == <undef'> assigned undefined  -> reads as undefined, `in` is TRUE
    tag == number   assigned a number   -> reads as the number

Where the middle one is worth spelling out, because it is the part that is easy to get
backwards: **an ordinary `undefined` JSValue has tag 0.** So tag 0 cannot mean "this
field holds undefined" - it is the value the field has when nothing was ever stored.
What a deliberately-assigned `undefined` needs is a *different* tag, which is what the
`<undef'>` above is: the value `undefined` normally carries, re-tagged with one of the
JSValue tag values that the engine does not otherwise use here.

Why that distinction has to exist: **the `in` operator**. `'x' in obj` is true exactly
when the property was set, including when it was set to `undefined`. If both states
shared tag 0, `in` could not tell them apart and a field that was explicitly cleared
would report as missing. The engine's `in` for this object is then answered from the
tag alone: non-zero means present.

It has the same shape once references exist: a field explicitly assigned `undefined`
does hold a value, so a later write knows there is a previous value to release. A field
that was never assigned holds nothing. tag 0 carries "nothing to release" with it.

The all-zero block is therefore exactly the "no property set" state, which is what
makes a single **memset** the whole initialisation - the same operation whether the
layout has three fields or thirty.

For the numeric-only first version this is about the tags alone, but the property is
what makes the general version possible, so the layout should be built to keep the
first 8 bytes of an unset field at zero from the start.
