#!/usr/bin/env bash
# Publishes docs/ to the SxfeScript/sxfescript.github.io repo, and installs
# install.sh there at both a version-pinned and a "latest" path so
#   curl -fsSL https://sxfescript.github.io/latest/install.sh | bash
# always gets the current script, while
#   curl -fsSL https://sxfescript.github.io/vX.Y.Z/install.sh | bash
# stays a stable pin to what shipped with that release.
#
# Usage: scripts/publish-docs.sh vX.Y.Z
#
# Env overrides:
#   SXN_DOCS_REPO   docs repo to publish to (default: SxfeScript/sxfescript.github.io)
#   SXN_REPO_URL    substituted for {{REPO_URL}} in docs/index.html
#                   (default: https://github.com/SxfeScript/sxfescript)

set -euo pipefail

VERSION="${1:?usage: scripts/publish-docs.sh vX.Y.Z}"
DOCS_REPO="${SXN_DOCS_REPO:-SxfeScript/sxfescript.github.io}"
REPO_URL="${SXN_REPO_URL:-https://github.com/SxfeScript/sxfescript}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git clone -q "https://github.com/$DOCS_REPO.git" "$tmp"

cp docs/logo.png "$tmp/logo.png"
sed "s#{{REPO_URL}}#$REPO_URL#g" docs/index.html > "$tmp/index.html"

mkdir -p "$tmp/latest" "$tmp/$VERSION"
cp install.sh "$tmp/latest/install.sh"
cp install.sh "$tmp/$VERSION/install.sh"
cp install.ps1 "$tmp/latest/install.ps1"
cp install.ps1 "$tmp/$VERSION/install.ps1"

cd "$tmp"
git add -A
if git diff --cached --quiet; then
  echo "docs repo already up to date, nothing to publish"
  exit 0
fi
git -c user.name="$(git -C "$ROOT" config user.name)" -c user.email="$(git -C "$ROOT" config user.email)" \
  commit -q -m "Publish docs, install.sh and install.ps1 for $VERSION"
git push -q origin main
echo "published docs + install.sh + install.ps1 ($VERSION and latest) to $DOCS_REPO"
