#!/usr/bin/env bash
# Installs the sxn binary release for the current platform.
#
#   curl -fsSL https://raw.githubusercontent.com/SxfeScript/sxfescript/main/install.sh | bash
#
# Override the version with SXN_VERSION (default: latest release tag),
# and the install directory with SXN_INSTALL (default: ~/.sxn).

set -euo pipefail

REPO="SxfeScript/sxfescript"
INSTALL_DIR="${SXN_INSTALL:-$HOME/.sxn}"
BIN_DIR="$INSTALL_DIR/bin"

fail() { echo "error: $1" >&2; exit 1; }

case "$(uname -s)" in
  Darwin) os=macos ;;
  Linux) os=linux ;;
  *) fail "unsupported OS: $(uname -s) (sxn ships macOS and Linux binaries; on Windows use install.ps1)" ;;
esac

case "$(uname -m)" in
  arm64|aarch64) arch=arm64 ;;
  x86_64|amd64) arch=x64 ;;
  *) fail "unsupported architecture: $(uname -m)" ;;
esac

version="${SXN_VERSION:-}"
if [ -z "$version" ]; then
  version="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name"' | sed -E 's/.*"tag_name": *"([^"]+)".*/\1/')"
  [ -n "$version" ] || fail "couldn't resolve the latest release; set SXN_VERSION to install a specific one"
fi

asset="sxn-${version#v}-${os}-${arch}.tar.gz"
url="https://github.com/$REPO/releases/download/$version/$asset"

echo "Installing sxn $version ($os-$arch)..."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$url" -o "$tmp/$asset" || fail "couldn't download $url (does this release ship a $os-$arch binary?)"

mkdir -p "$BIN_DIR"
tar -xzf "$tmp/$asset" -C "$tmp"
mv "$tmp/sxn-$os-$arch" "$BIN_DIR/sxn"
chmod +x "$BIN_DIR/sxn"

add_to_path() {
  rc="$1"
  [ -f "$rc" ] || return 0
  grep -qF "$BIN_DIR" "$rc" 2>/dev/null && return 0
  printf '\nexport PATH="%s:$PATH"\n' "$BIN_DIR" >> "$rc"
  echo "Added $BIN_DIR to PATH in $rc"
}

case "$(basename "${SHELL:-}")" in
  zsh) add_to_path "$HOME/.zshrc" ;;
  bash) add_to_path "$HOME/.bashrc"; add_to_path "$HOME/.bash_profile" ;;
  fish)
    mkdir -p "$HOME/.config/fish"
    add_to_path "$HOME/.config/fish/config.fish"
    ;;
  *) add_to_path "$HOME/.profile" ;;
esac

echo "sxn $version installed to $BIN_DIR/sxn"
echo "Open a new shell, or run: export PATH=\"$BIN_DIR:\$PATH\""
