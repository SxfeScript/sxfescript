#!/usr/bin/env bash
# Builds and packages the macOS/Linux targets reachable from this host and
# uploads any matching prebuilt Windows archives supplied in dist/. Windows
# x64/arm64 are cross-built separately; keep their binaries in dist/ before
# invoking this script so a release contains all six targets.
#
# Usage: scripts/release.sh vX.Y.Z [--prerelease]
#
# Env overrides:
#   SXN_REMOTE       ssh target for the Linux box (default: owner@10.0.0.43)
#   SXN_REMOTE_DIR    its checkout path (default: ~/sxfescript)
#   SXN_REPO          GitHub repo for the release (default: SxfeScript/sxfescript)

set -euo pipefail

VERSION="${1:?usage: scripts/release.sh vX.Y.Z [--prerelease]}"
PRERELEASE_FLAG=""
[ "${2:-}" = "--prerelease" ] && PRERELEASE_FLAG="--prerelease"

REMOTE="${SXN_REMOTE:-owner@10.0.0.43}"
REMOTE_DIR="${SXN_REMOTE_DIR:-~/sxfescript}"
REPO="${SXN_REPO:-SxfeScript/sxfescript}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p dist
rm -f dist/sxn-"${VERSION#v}"-*.tar.gz

pkg() { # pkg <bin-path> <strip-cmd> <os> <arch>
  local bin="$1" strip="$2" os="$3" arch="$4"
  local name="sxn-$os-$arch"
  "$strip" "$bin" -o "dist/$name"
  chmod +x "dist/$name"
  tar -C dist -czf "dist/sxn-${VERSION#v}-$os-$arch.tar.gz" "$name"
  rm "dist/$name"
  echo "packaged dist/sxn-${VERSION#v}-$os-$arch.tar.gz"
}

echo "== macOS arm64 (native) =="
cmake --preset release >/dev/null
cmake --build --preset release
ctest --test-dir build/release --output-on-failure
pkg build/release/sxn strip macos arm64

echo "== macOS x64 (Rosetta cross-build) =="
if ! arch -x86_64 /usr/local/bin/brew --version >/dev/null 2>&1; then
  echo "skip: no x86_64 Homebrew at /usr/local (arch -x86_64 brew install openssl@3 libuv libffi)" >&2
else
  rm -rf build/release-x64
  cmake -S . -B build/release-x64 -DCMAKE_BUILD_TYPE=Release -DSXN_ENABLE_POISON=OFF \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_PREFIX_PATH=/usr/local \
    -DOPENSSL_ROOT_DIR=/usr/local/opt/openssl@3 >/dev/null
  cmake --build build/release-x64
  ctest --test-dir build/release-x64 --output-on-failure
  pkg build/release-x64/sxn strip macos x64
fi

echo "== Linux x64 (native, on $REMOTE) =="
rsync -az --exclude 'build/' --exclude '.git/' --exclude 'dist/' ./ "$REMOTE:$REMOTE_DIR/"
ssh "$REMOTE" "cd $REMOTE_DIR && cmake --preset release >/dev/null && cmake --build --preset release && ctest --test-dir build/release --output-on-failure"
ssh "$REMOTE" "strip $REMOTE_DIR/build/release/sxn -o /tmp/sxn-linux-x64"
scp "$REMOTE:/tmp/sxn-linux-x64" dist/sxn-linux-x64
chmod +x dist/sxn-linux-x64
tar -C dist -czf "dist/sxn-${VERSION#v}-linux-x64.tar.gz" sxn-linux-x64
rm dist/sxn-linux-x64
ssh "$REMOTE" "rm -f /tmp/sxn-linux-x64"
echo "packaged dist/sxn-${VERSION#v}-linux-x64.tar.gz"

echo "== Linux arm64 (cross-compiled on $REMOTE) =="
if ! ssh "$REMOTE" "command -v aarch64-linux-gnu-gcc" >/dev/null 2>&1; then
  echo "skip: no aarch64 cross toolchain on $REMOTE (see cmake/aarch64-linux.cmake's header for the apt packages needed)" >&2
else
  ssh "$REMOTE" "cd $REMOTE_DIR && rm -rf build/release-arm64 && \
    PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig PKG_CONFIG_SYSROOT_DIR=/ \
    cmake -S . -B build/release-arm64 -DCMAKE_BUILD_TYPE=Release -DSXN_ENABLE_POISON=OFF \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux.cmake \
      -DOPENSSL_INCLUDE_DIR=/usr/include -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/aarch64-linux-gnu/libcrypto.so \
      -DOPENSSL_SSL_LIBRARY=/usr/lib/aarch64-linux-gnu/libssl.so \
      -DCURL_INCLUDE_DIR=/usr/include/aarch64-linux-gnu -DCURL_LIBRARY=/usr/lib/aarch64-linux-gnu/libcurl.so \
      -DZLIB_INCLUDE_DIR=/usr/include -DZLIB_LIBRARY=/usr/lib/aarch64-linux-gnu/libz.so >/dev/null"
  # qjsc runs at build time to generate bytecode headers; the freshly cross-built one
  # is aarch64 and can't execute on this x64 host, so borrow the native qjsc built
  # above for that step (bytecode is arch-independent - see CMakeLists.txt's note
  # on why the compiler must come from this exact source tree).
  ssh "$REMOTE" "cd $REMOTE_DIR && cmake --build build/release-arm64 --target qjsc >/dev/null && \
    cp build/release/third_party/arcsx/qjsc build/release-arm64/third_party/arcsx/qjsc && \
    touch build/release-arm64/third_party/arcsx/qjsc"
  ssh "$REMOTE" "cd $REMOTE_DIR && PATH=\"$REMOTE_DIR/build/release-arm64/third_party/arcsx:\$PATH\" cmake --build build/release-arm64"
  ssh "$REMOTE" "aarch64-linux-gnu-strip $REMOTE_DIR/build/release-arm64/sxn -o /tmp/sxn-linux-arm64"
  scp "$REMOTE:/tmp/sxn-linux-arm64" dist/sxn-linux-arm64
  chmod +x dist/sxn-linux-arm64
  tar -C dist -czf "dist/sxn-${VERSION#v}-linux-arm64.tar.gz" sxn-linux-arm64
  rm dist/sxn-linux-arm64
  ssh "$REMOTE" "rm -f /tmp/sxn-linux-arm64"
  echo "packaged dist/sxn-${VERSION#v}-linux-arm64.tar.gz (not execution-tested here - no arm64 host)"
fi

echo "== Uploading to $REPO release $VERSION =="
assets=(dist/sxn-"${VERSION#v}"-*.tar.gz dist/sxn-"${VERSION#v}"-*.zip)
if gh release view "$VERSION" --repo "$REPO" >/dev/null 2>&1; then
  gh release upload "$VERSION" "${assets[@]}" --repo "$REPO" --clobber
else
  gh release create "$VERSION" "${assets[@]}" --repo "$REPO" --title "$VERSION" $PRERELEASE_FLAG
fi
echo "done: ${#assets[@]} asset(s) uploaded to $VERSION"
