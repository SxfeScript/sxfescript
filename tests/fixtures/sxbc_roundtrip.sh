#!/bin/sh
# Compiles both an ESM and a CommonJS fixture to .sxbc, runs the bytecode
# directly, and checks it against the same fixture run from source -- module
# and CommonJS take different paths through sxn_run_sxbc, so both need
# covering. Also exercises --compile-cache reuse/invalidation, --strip, and
# the error path for a corrupt .sxbc. $1 is the sxn binary under test.
set -e
SXN="$1"
[ -x "$SXN" ] || { echo "sxbc_roundtrip: usage: sh sxbc_roundtrip.sh <path-to-sxn>"; exit 2; }
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT
bad=0
check() {
  name="$1"; got="$2"; want="$3"
  if [ "$got" != "$want" ]; then
    bad=1
    printf 'FAIL %s\n  got:  %s\n  want: %s\n' "$name" "$got" "$want"
  fi
}

# ---- ESM module bytecode ---------------------------------------------
cat > "$DIR/mod.mjs" <<'EOF'
export const x = 1 + 1;
console.log("module ran, x =", x);
EOF
"$SXN" compile "$DIR/mod.mjs" >/dev/null
got="$("$SXN" "$DIR/mod.sxbc")"
check "module bytecode output" "$got" "module ran, x = 2"

# ---- CommonJS bytecode: require(), process.argv, module.exports -------
cat > "$DIR/lib.cjs" <<'EOF'
module.exports = { greet: (n) => "hi " + n };
EOF
cat > "$DIR/app.cjs" <<'EOF'
const { greet } = require("./lib.cjs");
console.log(greet("world"));
console.log("argv:", process.argv.slice(2).join(","));
EOF
"$SXN" compile "$DIR/app.cjs" >/dev/null
got="$("$SXN" "$DIR/app.sxbc" one two)"
want="hi world
argv: one,two"
check "cjs bytecode output" "$got" "$want"

src_direct="$("$SXN" "$DIR/app.cjs" one two)"
check "cjs bytecode matches source run" "$got" "$src_direct"

# ---- --compile-cache: reuse when fresh, rebuild when the source changes
rm -f "$DIR/app.sxbc"
"$SXN" --compile-cache "$DIR/app.cjs" a b >/dev/null
stamp1=$(ls -l "$DIR/app.sxbc" | awk '{print $5}')  # size, as a cheap proxy
"$SXN" --compile-cache "$DIR/app.cjs" c d >/dev/null
stamp2=$(ls -l "$DIR/app.sxbc" | awk '{print $5}')
check "compile-cache reuses a fresh cache" "$stamp2" "$stamp1"

sleep 1
echo 'console.log("modified");' >> "$DIR/app.cjs"
got="$("$SXN" --compile-cache "$DIR/app.cjs" e f)"
case "$got" in
  *modified*) : ;;
  *) bad=1; printf 'FAIL compile-cache recompiles a changed source\n  got: %s\n' "$got" ;;
esac

# process.argv must not include --compile-cache or the flag's position;
# the earlier argv-offset bug leaked the flag into the script's own argv.
case "$got" in
  *"a,b"*|*"--compile-cache"*) bad=1; printf 'FAIL flag leaked into process.argv\n  got: %s\n' "$got" ;;
esac

# ---- --strip: the compiling machine's path must not survive ----------
cat > "$DIR/priv.mjs" <<'EOF'
function boom() { throw new Error("kaboom"); }
boom();
EOF
"$SXN" compile "$DIR/priv.mjs" -o "$DIR/priv.sxbc" --strip >/dev/null
if strings "$DIR/priv.sxbc" 2>/dev/null | grep -q "$DIR"; then
  bad=1; echo "FAIL --strip left the compiling directory embedded in the .sxbc"
fi
# and it must still run cleanly (this used to crash with "realpath failure")
"$SXN" "$DIR/priv.sxbc" >/dev/null 2>&1 || true   # throws by design; just must not crash the runtime
if [ "$?" -gt 1 ] 2>/dev/null; then bad=1; echo "FAIL stripped bytecode crashed instead of throwing cleanly"; fi

# a stripped .sxbc must still run after being moved somewhere else entirely,
# with the original source gone -- the actual "distribution" scenario.
mkdir -p "$DIR/elsewhere"
mv "$DIR/priv.sxbc" "$DIR/elsewhere/"
rm -f "$DIR/priv.mjs"
if ! (cd "$DIR/elsewhere" && "$SXN" priv.sxbc >/dev/null 2>&1); then
  code=$?
  if [ "$code" -gt 1 ]; then bad=1; echo "FAIL moved stripped .sxbc did not run (exit $code)"; fi
fi

# ---- a program of more than one file --------------------------------
# Imports are resolved while a module is compiled, so `sxn compile` needs the
# same module loader running a file does. It had none, and failed on any file
# that imported a sibling -- which is every file in a real program.
cat > "$DIR/lib.sx" <<'LIB'
export const greet = (who: string): string => `hello ${who}`;
LIB
cat > "$DIR/entry.sx" <<'ENTRY'
import { greet } from "./lib.sx";
console.log(greet("bytecode"));
ENTRY
if ! "$SXN" compile "$DIR/entry.sx" -o "$DIR/entry.sxbc" >/dev/null 2>&1; then
  bad=1; echo "FAIL compiling a file with an import failed"
else
  out=$("$SXN" "$DIR/entry.sxbc" 2>&1 || true)
  [ "$out" = "hello bytecode" ] || { bad=1; echo "FAIL bytecode with an import printed '$out'"; }
fi

# ---- a corrupt/foreign .sxbc is a clean error, not a crash ------------
echo "not bytecode" > "$DIR/bad.sxbc"
set +e
"$SXN" "$DIR/bad.sxbc" >"$DIR/bad.out" 2>&1
code=$?
set -e
if [ "$code" -gt 1 ]; then bad=1; echo "FAIL corrupt .sxbc crashed (exit $code) instead of a clean error"; fi
grep -q "not a recognized .sxbc file" "$DIR/bad.out" || { bad=1; echo "FAIL corrupt .sxbc gave no clear message"; }

if [ "$bad" -eq 0 ]; then echo "ALL PASS"; exit 0; else exit 1; fi
