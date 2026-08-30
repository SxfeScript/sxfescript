// Sxn.ffi against the platform's own C library: no build step, and the
// functions are the same everywhere.
let bad = 0;
// BigInt has no JSON form, and half of what an FFI returns is one.
const show = (v) => JSON.stringify(v, (_, x) => (typeof x === "bigint" ? x + "n" : x));
const check = (n, got, want) => { const g = show(got);
  if (g !== show(want)) { bad++; console.log("FAIL " + n + " got=" + g + " want=" + show(want)); } };

const libc = process.platform === "darwin" ? "libSystem.B.dylib"
  : process.platform === "win32" ? "msvcrt.dll" : "libc.so.6";
// msvcrt.dll exports the POSIXish ones with a leading underscore.
const getpid_sym = process.platform === "win32" ? "_getpid" : "getpid";
const libm = process.platform === "darwin" ? "libSystem.B.dylib"
  : process.platform === "win32" ? "msvcrt.dll" : "libm.so.6";

check("f64 in and out", Sxn.ffi(libm, "pow", ["f64", "f64"], "f64")(2, 10), 1024);
check("i32 in and out", Sxn.ffi(libc, "abs", ["i32"], "i32")(-7), 7);
check("negative result", Sxn.ffi(libc, "abs", ["i32"], "i32")(7), 7);

// A 64-bit result is a BigInt, because a double cannot carry one exactly.
const strlen = Sxn.ffi(libc, "strlen", ["cstring"], "u64");
check("cstring argument", strlen("hello"), 5n);
check("u64 is a BigInt", typeof strlen("x"), "bigint");
check("empty string", strlen(""), 0n);
check("utf-8 is bytes, not characters", strlen("héllo"), 6n);

// A returned char* becomes a string; NULL becomes null rather than "".
const getenv = Sxn.ffi(libc, "getenv", ["cstring"], "cstring");
check("cstring return", typeof getenv("PATH"), "string");
check("NULL return is null", getenv("SXN_DEFINITELY_NOT_SET_XYZ"), null);

// A typed array passes the address of its own bytes, so out-parameters work.
const memset = Sxn.ffi(libc, "memset", ["pointer", "i32", "u64"], "pointer");
const buf = new Uint8Array(8);
memset(buf, 0x41, 8n);
check("writes through a pointer", Array.from(buf), [65, 65, 65, 65, 65, 65, 65, 65]);
// A subarray must pass its own offset, not the start of the buffer.
memset(buf.subarray(4), 0x5a, 4n);
check("respects a view's offset", Array.from(buf), [65, 65, 65, 65, 90, 90, 90, 90]);

const memcmp = Sxn.ffi(libc, "memcmp", ["pointer", "pointer", "u64"], "i32");
check("null pointer argument", memcmp(null, null, 0n), 0);

// An empty library name means this executable.
check("self lookup", Sxn.ffi("", "strlen", ["cstring"], "u64")("abcd"), 4n);

const errs = [];
const trap = (f) => { try { f(); return "no throw"; } catch (e) { return e.constructor.name; } };
errs.push(["missing symbol", trap(() => Sxn.ffi(libc, "sxn_no_such_symbol", [], "void"))]);
errs.push(["missing library", trap(() => Sxn.ffi("libsxn_does_not_exist.so", "x", [], "void"))]);
errs.push(["unknown type", trap(() => Sxn.ffi(libc, "abs", ["struct"], "i32"))]);
errs.push(["void as an argument", trap(() => Sxn.ffi(libc, "memcmp", ["void", "i32"], "i32"))]);
check("failures are errors, not crashes", errs,
      [["missing symbol", "ReferenceError"], ["missing library", "ReferenceError"],
       ["unknown type", "TypeError"], ["void as an argument", "TypeError"]]);

// `void` alone is C's way of writing "takes nothing".
check("void argument list means none", typeof Sxn.ffi(libc, getpid_sym, ["void"], "i32")(), "number");
check("too few arguments throws", trap(() => Sxn.ffi(libm, "pow", ["f64", "f64"], "f64")(2)), "TypeError");

console.log(bad === 0 ? "ALL PASS" : "FAILURES: " + bad);
process.exit(bad === 0 ? 0 : 1);
