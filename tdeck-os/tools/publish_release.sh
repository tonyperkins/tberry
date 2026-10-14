#!/bin/sh
# Build, sign and publish a firmware release to GitHub. Version and build number come from
# include/version.h (raise TDOS_BUILD for every release, or devices will not install it).
#
#   tools/publish_release.sh "one-line release notes"
#   DRY_RUN=1 tools/publish_release.sh   builds and signs into dist/release without publishing
#
# Needs: the signing key (tools/sign_release.py keygen), `gh` logged in, and a clean `pio` setup.
# Devices fetch .../releases/latest/download/manifest.json, so the newest release is what they see.
set -e
cd "$(dirname "$0")/.."
NOTES="${1:-Firmware release}"
VERSION=$(sed -n 's/^#define TDOS_VERSION "\(.*\)"/\1/p' include/version.h)
BUILD=$(sed -n 's/^#define TDOS_BUILD \([0-9]*\)/\1/p' include/version.h)
REPO=$(git config --get remote.origin.url | sed -e 's#.*github.com[:/]##' -e 's#\.git$##')
TAG="v$VERSION"
[ -n "$VERSION" ] && [ -n "$BUILD" ] || { echo "cannot read version from include/version.h" >&2; exit 1; }

# The key compiled into the firmware must be the one that signs this release.
python3 tools/sign_release.py header /tmp/update_key_check.h >/dev/null
cmp -s /tmp/update_key_check.h include/update_key.h || { echo "include/update_key.h does not match the signing key" >&2; exit 1; }

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  echo "release $TAG already exists; raise TDOS_VERSION and TDOS_BUILD" >&2
  exit 1
fi

./tools/release.sh
OUT=dist/release
mkdir -p "$OUT"
cp .pio/build/tdeck/firmware.bin "$OUT/firmware.bin"
URL="https://github.com/$REPO/releases/download/$TAG/firmware.bin"
python3 tools/sign_release.py sign "$OUT/firmware.bin" --version "$VERSION" --build "$BUILD" --url "$URL" --out "$OUT/manifest.json"

if [ -n "$DRY_RUN" ]; then
  echo "dry run: built and signed $TAG (build $BUILD) in $OUT, nothing published"
  exit 0
fi

echo "Publishing $TAG (build $BUILD) to $REPO"
gh release create "$TAG" "$OUT/firmware.bin" "$OUT/manifest.json" --repo "$REPO" --title "tdeck-os $VERSION" --notes "$NOTES"
