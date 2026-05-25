#!/bin/sh
# Install vstbridgectl-gtk and its desktop entry.
#
# Usage:
#   ./install.sh            # user install  (~/.local/bin, ~/.local/share/applications)
#   ./install.sh --system   # system install (/usr/local/bin, /usr/share/applications)
#   sudo ./install.sh       # system install (detected via root UID)
#   ./install.sh --uninstall [--system]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/build/vstbridgectl-gtk"
DESKTOP_SRC="$SCRIPT_DIR/vstbridgectl-gtk.desktop"

# ── Parse arguments ──────────────────────────────────────────────────────────

SYSTEM=0
UNINSTALL=0

for arg in "$@"; do
    case "$arg" in
        --system)   SYSTEM=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --help|-h)
            echo "Usage: $0 [--system] [--uninstall]"
            echo "  (no flags)   Install for current user"
            echo "  --system     Install system-wide (or run as root)"
            echo "  --uninstall  Remove installed files"
            exit 0
            ;;
    esac
done

# Root always means system install
if [ "$(id -u)" -eq 0 ]; then
    SYSTEM=1
fi

if [ "$SYSTEM" -eq 1 ]; then
    BIN_DIR="/usr/local/bin"
    APPS_DIR="/usr/share/applications"
else
    BIN_DIR="${XDG_DATA_HOME:-$HOME/.local}/bin"
    APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
fi

# ── Uninstall ────────────────────────────────────────────────────────────────

if [ "$UNINSTALL" -eq 1 ]; then
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

# ── Build if needed ──────────────────────────────────────────────────────────

if [ ! -f "$BINARY" ]; then
    echo "Binary not found — building..."
    make -C "$SCRIPT_DIR"
fi

# ── Install binary ───────────────────────────────────────────────────────────

mkdir -p "$BIN_DIR"
cp "$BINARY" "$BIN_DIR/vstbridgectl-gtk"
chmod 755 "$BIN_DIR/vstbridgectl-gtk"
echo "Installed binary:       $BIN_DIR/vstbridgectl-gtk"

# ── Install desktop entry ────────────────────────────────────────────────────

mkdir -p "$APPS_DIR"
cp "$DESKTOP_SRC" "$APPS_DIR/vstbridgectl-gtk.desktop"
echo "Installed desktop file: $APPS_DIR/vstbridgectl-gtk.desktop"

# ── Refresh desktop database ─────────────────────────────────────────────────

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPS_DIR" 2>/dev/null || true
fi

echo "Done."

# Warn if user install bin dir is not in PATH
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
