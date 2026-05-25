#!/bin/sh
# vstbridge installer — installs pre-built binaries from a release tarball.
#
# Usage:
#   ./install.sh              # user install  (~/.local/...)
#   ./install.sh --system     # system-wide   (/usr/local/bin, /usr/share/...)
#   sudo ./install.sh         # system-wide   (detected via root UID)
#   ./install.sh --uninstall [--system]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Parse arguments ──────────────────────────────────────────────────────────

SYSTEM=0
UNINSTALL=0

for arg in "$@"; do
    case "$arg" in
        --system)    SYSTEM=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --help|-h)
            echo "Usage: $0 [--system] [--uninstall]"
            echo "  (no flags)   Install for the current user"
            echo "  --system     Install system-wide (or run as root)"
            echo "  --uninstall  Remove installed files"
            exit 0
            ;;
    esac
done

if [ "$(id -u)" -eq 0 ]; then
    SYSTEM=1
fi

if [ "$SYSTEM" -eq 1 ]; then
    BIN_DIR="/usr/local/bin"
    APPS_DIR="/usr/share/applications"
    DATA_DIR="/usr/local/share/vstbridge"
else
    BIN_DIR="${XDG_DATA_HOME:-$HOME/.local}/bin"
    APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/vstbridge"
fi

# ── Uninstall ────────────────────────────────────────────────────────────────

if [ "$UNINSTALL" -eq 1 ]; then
    echo "Removing bridge files from $DATA_DIR"
    rm -f \
        "$DATA_DIR"/libvstbridge-vst2.so \
        "$DATA_DIR"/libvstbridge-vst3.so \
        "$DATA_DIR"/libvstbridge-clap.so \
        "$DATA_DIR"/libvstbridge-chainloader-vst2.so \
        "$DATA_DIR"/libvstbridge-chainloader-vst3.so \
        "$DATA_DIR"/libvstbridge-chainloader-clap.so \
        "$DATA_DIR"/vstbridge-host.exe \
        "$DATA_DIR"/vstbridge-host.exe.so \
        "$DATA_DIR"/vstbridge-host-32.exe \
        "$DATA_DIR"/vstbridge-host-32.exe.so

    echo "Removing $BIN_DIR/vstbridgectl"
    rm -f "$BIN_DIR/vstbridgectl"
    echo "Removing $BIN_DIR/vstbridgectl-gtk"
    rm -f "$BIN_DIR/vstbridgectl-gtk"
    echo "Removing $APPS_DIR/vstbridgectl-gtk.desktop"
    rm -f "$APPS_DIR/vstbridgectl-gtk.desktop"

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$APPS_DIR" 2>/dev/null || true
    fi
    echo "Done."
    exit 0
fi

# ── Verify we are running from the extracted tarball ─────────────────────────

for f in \
    libvstbridge-vst2.so \
    libvstbridge-chainloader-vst2.so \
    vstbridge-host.exe \
    vstbridgectl \
    vstbridgectl-gtk
do
    if [ ! -f "$SCRIPT_DIR/$f" ]; then
        echo "Error: $f not found in $SCRIPT_DIR" >&2
        echo "Run this script from the extracted vstbridge release directory." >&2
        exit 1
    fi
done

# ── Install bridge data files ─────────────────────────────────────────────────

mkdir -p "$DATA_DIR"

for f in \
    libvstbridge-vst2.so \
    libvstbridge-vst3.so \
    libvstbridge-clap.so \
    libvstbridge-chainloader-vst2.so \
    libvstbridge-chainloader-vst3.so \
    libvstbridge-chainloader-clap.so \
    vstbridge-host.exe \
    vstbridge-host.exe.so \
    vstbridge-host-32.exe \
    vstbridge-host-32.exe.so
do
    if [ -f "$SCRIPT_DIR/$f" ]; then
        cp "$SCRIPT_DIR/$f" "$DATA_DIR/$f"
        chmod 755 "$DATA_DIR/$f"
    fi
done

echo "Installed bridge files to: $DATA_DIR"

# ── Install CLI tool ──────────────────────────────────────────────────────────

mkdir -p "$BIN_DIR"
cp "$SCRIPT_DIR/vstbridgectl" "$BIN_DIR/vstbridgectl"
chmod 755 "$BIN_DIR/vstbridgectl"
echo "Installed:                 $BIN_DIR/vstbridgectl"

# ── Install GUI tool ──────────────────────────────────────────────────────────

cp "$SCRIPT_DIR/vstbridgectl-gtk" "$BIN_DIR/vstbridgectl-gtk"
chmod 755 "$BIN_DIR/vstbridgectl-gtk"
echo "Installed:                 $BIN_DIR/vstbridgectl-gtk"

# ── Install desktop entry ─────────────────────────────────────────────────────

mkdir -p "$APPS_DIR"
cp "$SCRIPT_DIR/vstbridgectl-gtk.desktop" "$APPS_DIR/vstbridgectl-gtk.desktop"
echo "Installed:                 $APPS_DIR/vstbridgectl-gtk.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPS_DIR" 2>/dev/null || true
fi

# ── Done ──────────────────────────────────────────────────────────────────────

echo ""
echo "vstbridge installed successfully."
echo ""
echo "Next steps:"
echo "  1. Tell vstbridgectl where vstbridge is installed:"
echo "     vstbridgectl set --path=$DATA_DIR"
echo "  2. Add your Windows plugin directories, for example:"
echo "     vstbridgectl add ~/.wine/drive_c/Program\ Files/VstPlugins"
echo "  3. Sync plugins:"
echo "     vstbridgectl sync"
echo "  Or open the GUI:  vstbridgectl-gtk"

# Warn if user bin dir is not in PATH
if [ "$SYSTEM" -eq 0 ]; then
    case ":$PATH:" in
        *":$BIN_DIR:"*) ;;
        *)
            echo ""
            echo "Note: $BIN_DIR is not in your PATH."
            echo "Add this to your shell profile (~/.profile or ~/.bashrc):"
            echo "  export PATH=\"\$PATH:$BIN_DIR\""
            ;;
    esac
fi
