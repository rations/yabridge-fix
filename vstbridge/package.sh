#!/bin/sh
# Create a vstbridge release tarball from already-built binaries.
#
# Run this from the vstbridge source root after a successful build:
#   meson setup build --buildtype=release --cross-file=cross-wine.conf -Dbitbridge=true
#   ninja -C build
#   cd tools/vstbridgectl && make && cd ../..
#   ./package.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TOOL_BUILD_DIR="$SCRIPT_DIR/tools/vstbridgectl/build"

# ── Determine version ────────────────────────────────────────────────────────

VERSION="$1"
if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version>  (e.g. $0 0.0.1)" >&2
    exit 1
fi
ARCHIVE="vstbridge-$VERSION.tar.gz"

echo "Packaging vstbridge $VERSION -> $ARCHIVE"

# ── Verify required build outputs exist ──────────────────────────────────────

missing=0
for f in \
    "$BUILD_DIR/libvstbridge-vst2.so" \
    "$BUILD_DIR/libvstbridge-vst3.so" \
    "$BUILD_DIR/libvstbridge-clap.so" \
    "$BUILD_DIR/libvstbridge-chainloader-vst2.so" \
    "$BUILD_DIR/libvstbridge-chainloader-vst3.so" \
    "$BUILD_DIR/libvstbridge-chainloader-clap.so" \
    "$BUILD_DIR/vstbridge-host.exe" \
    "$BUILD_DIR/vstbridge-host.exe.so" \
    "$TOOL_BUILD_DIR/vstbridgectl" \
    "$TOOL_BUILD_DIR/vstbridgectl-gtk"
do
    if [ ! -f "$f" ]; then
        echo "Missing: $f" >&2
        missing=1
    fi
done

if [ "$missing" -eq 1 ]; then
    echo "" >&2
    echo "Build is incomplete. Run:" >&2
    echo "  meson setup build --buildtype=release --cross-file=cross-wine.conf -Dbitbridge=true" >&2
    echo "  ninja -C build" >&2
    echo "  cd tools/vstbridgectl && make && cd ../.." >&2
    exit 1
fi

# ── Assemble staging directory ────────────────────────────────────────────────

STAGING="$(mktemp -d)/vstbridge"
mkdir -p "$STAGING"

# Bridge data files
cp "$BUILD_DIR/libvstbridge-vst2.so"              "$STAGING/"
cp "$BUILD_DIR/libvstbridge-vst3.so"              "$STAGING/"
cp "$BUILD_DIR/libvstbridge-clap.so"              "$STAGING/"
cp "$BUILD_DIR/libvstbridge-chainloader-vst2.so"  "$STAGING/"
cp "$BUILD_DIR/libvstbridge-chainloader-vst3.so"  "$STAGING/"
cp "$BUILD_DIR/libvstbridge-chainloader-clap.so"  "$STAGING/"
cp "$BUILD_DIR/vstbridge-host.exe"                "$STAGING/"
cp "$BUILD_DIR/vstbridge-host.exe.so"             "$STAGING/"

# 32-bit host (present when built with -Dbitbridge=true)
for f in vstbridge-host-32.exe vstbridge-host-32.exe.so; do
    if [ -f "$BUILD_DIR/$f" ]; then
        cp "$BUILD_DIR/$f" "$STAGING/"
    fi
done

# vstbridgectl tools
cp "$TOOL_BUILD_DIR/vstbridgectl"     "$STAGING/"
cp "$TOOL_BUILD_DIR/vstbridgectl-gtk" "$STAGING/"

# Desktop entry, installer, and docs
cp "$SCRIPT_DIR/tools/vstbridgectl/vstbridgectl-gtk.desktop" "$STAGING/"
cp "$SCRIPT_DIR/install.sh"    "$STAGING/"
cp "$REPO_DIR/README.md"      "$STAGING/"
cp "$SCRIPT_DIR/CHANGELOG.md" "$STAGING/"

chmod +x "$STAGING/install.sh"

# ── Create archive ────────────────────────────────────────────────────────────

tar -caf "$SCRIPT_DIR/$ARCHIVE" -C "$(dirname "$STAGING")" vstbridge
rm -rf "$(dirname "$STAGING")"

echo "Created: $SCRIPT_DIR/$ARCHIVE"
echo ""
echo "Contents:"
tar -tf "$SCRIPT_DIR/$ARCHIVE"
