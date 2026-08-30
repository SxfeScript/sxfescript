// Calling a C function directly. Run it: sxn examples/ffi.mjs
//
// Sxn.ffi(library, symbol, argumentTypes, returnType) returns a callable
// JavaScript function, through libffi and dlopen. This is an engine
// capability, not a Node one -- see spec/NATIVE.md for the type list and for
// what is deliberately unsupported (structs by value, callbacks, variadics).

const libm = {
  darwin: "libSystem.B.dylib",
  linux: "libm.so.6",
  win32: "msvcrt.dll",
}[process.platform];

if (!libm) throw new Error(`no libm name known for ${process.platform}`);

const pow = Sxn.ffi(libm, "pow", ["f64", "f64"], "f64");
const sqrt = Sxn.ffi(libm, "sqrt", ["f64"], "f64");

console.log("pow(2, 10) =", pow(2, 10));
console.log("sqrt(144)  =", sqrt(144));
