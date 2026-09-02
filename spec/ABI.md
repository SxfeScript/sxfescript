# Sxfe host ABI

The stable public C surface starts in `include/sxfe.h`. Layout descriptors are
finalized before registration. Typed native calls receive pointers only for the
duration of the call. Shared pointers are read-only; mutable pointers are
exclusive. Hosts must not retain either pointer.

The production engine will expose distinct `sx_*` bytecodes for allocation,
move, shared borrow, mutable borrow, release, field access, and owned drop.
QuickJS `OP_drop` remains untouched because it is an operand-stack operation.

None of those exist yet, and the ownership rules are still erased at runtime:
`&mut` aliases through JavaScript object identity, so it mutates a struct in
place but cannot write back to a caller's number. One rule is enforced ahead
of them at parse time -- `&mut` requires a `let mut` owner, SX2003 -- and the
declared scalar types are recorded rather than stripped, which is what gates
the i32 arithmetic semantics and the typed-call inlining. `spec/LANGUAGE.md`
has the contract and `spec/IMPLEMENTATION.md` has what is checked today.

The allocation bytecode is the one the compiler has so far been able to do
without rather than add: a struct whose every use the compiler can account for
is split into scalar locals and never allocated, and one that escapes is built
by the ordinary object opcodes at the point it escapes. An `sx_alloc` earns
its slot when a struct that must exist as an object is common enough to
measure, not before.

Moving a layout value into JavaScript consumes it and boxes a copy. Borrowing it
into JavaScript creates a revocable proxy. Typed JavaScript objects crossing
into SX use shared/exclusive header locks, and incompatible property writes
throw `TypeError`.

FFI declarations are unsafe by default and are expected to use the platform C
ABI. The initial syntax is `unsafe extern name(types): return from "library"`.
Library handles must be runtime-owned and remain live until all wrappers and
callbacks are released; native pointers may not outlive a call unless an
explicit ownership type is added. What is implemented today, and where it
sits relative to the Node compatibility layer, is `spec/NATIVE.md`.
