# Working rules for this repository

These are in addition to the *Rules for AI-Written Code* that `COMPLIANCE.md` records.

## Fill every field of a record, always

When constructing anything the OS or a library will read — a window class, a font, a tooltip, a device
description, a request record, a control — name **every** field, including the ones that want nothing.
Write the zero, write the `nullptr`, write the default. Do not rely on `{}` to mean the right thing, and do
not leave a field out because its default is already what you want.

Use designated initialisers where the type allows them, so each value is next to the name it belongs to. A
record with an anonymous union in it cannot be designated; there, zero-initialise and then assign every
field by name, with an `R1` waiver saying that is why the function is long.

The reason is this project's own history. Controls have gone missing, arrows have done nothing and a window
class has behaved oddly because a field nobody thought about was left at whatever a zeroed record happens
to mean. A field written down is a field that was decided.

## Do not hand out a view of a temporary

`std::span` and `std::wstring_view` keep no ownership. A span built from a function that returns by value
dangles the moment the full expression ends. If a helper returns an array by value, use it inside the
expression that asks for it, or store the array first. This has caused a start-up failure here once already.

## Windows-only code is not covered by the tests

`src/effects/real` is compiled by the MinGW harness and built by CI, but nothing runs it except a person on
Windows. Reason about it as though there is no safety net, because there is not.
