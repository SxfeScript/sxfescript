# Sxfe host ABI

The stable public C surface starts in `include/sxfe.h`. Layout descriptors are
finalized before registration. Typed native calls receive pointers only for the
duration of the call. Shared pointers are read-only; mutable pointers are
exclusive. Hosts must not retain either pointer.

The production engine will expose distinct `sx_*` bytecodes for allocation,
move, shared borrow, mutable borrow, release, field access, and owned drop.
QuickJS `OP_drop` remains untouched because it is an operand-stack operation.

Moving a layout value into JavaScript consumes it and boxes a copy. Borrowing it
into JavaScript creates a revocable proxy. Typed JavaScript objects crossing
into SX use shared/exclusive header locks, and incompatible property writes
throw `TypeError`.

FFI declarations are unsafe by default and are expected to use the platform C
ABI. The initial syntax is `unsafe extern name(types): return from "library"`.
Library handles must be runtime-owned and remain live until all wrappers and
callbacks are released; native pointers may not outlive a call unless an
explicit ownership type is added.
