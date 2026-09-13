# Unboxed struct initialisation: memoise the block and let tag 0 mean "unset"

Raised by the user, and it removes the per-field initialisation cost.

Every property of a fresh unboxed object reads as `undefined` when it was never
assigned, which is what makes the behaviour match an ordinary object. The obvious way
to get that is to write `undefined` into every field on creation - one store per field
per object.

Instead, **separate "never assigned" from "assigned the value undefined"**, and do it
with the tag:

    tag == 0        never assigned      -> reads as undefined
    tag == <undef>  assigned undefined  -> reads as undefined
    tag == number   assigned a number   -> reads as the number

`0` is a value the real tag space does not use - it is not a JSValue tag - so it is
free to mean "unset", and the empty state is then all-zero bytes. That means the whole
block can be initialised with **one memset**, rather than one store per field, and it
is the same operation whether the layout has three fields or thirty.

The two cases stay distinguishable afterwards: a field that was explicitly assigned
`undefined` has a non-zero tag, so a write to it still knows there is a previous value
to release, and a field that was never assigned has nothing to release. That is the
distinction the reference-counting rule needs - without it, "is there a value to free
here" would have to be answered some other way, or freeing would be attempted on
garbage.

For the numeric-only first version this is about the tags alone, but the property is
what makes the general version possible, so the layout should be built to keep the
first 8 bytes of an unset field at zero from the start.
