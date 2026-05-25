# vstbridge — CLAUDE.md

## What this project is

vstbridge is a fork of [yabridge](https://github.com/robbert-vdh/yabridge) 5.1.1 — a Wine-based bridge that allows Linux DAWs to load Windows VST2, VST3, and CLAP plugins. This fork adds:

1. **A critical Wine ≥9.21 fix** — plugin GUIs were rendered but completely non-interactive due to a `wm_state_serial` deadlock in Wine's XEmbed path. Fixed via `SetWindowPos()` in `src/wine-host/editor.cpp` and XEmbed enabled by default in `src/common/configuration.h`.
2. **A GTK3 GUI for vstbridgectl** — added in `tools/vstbridgectl/src/gui/` (the original was CLI-only).
3. **Complete rename** from `yabridge` → `vstbridge` throughout the entire codebase.

The project is licensed under GPL-3.0 (same as upstream). Copyright in individual files is retained by the original author (Robbert van der Helm).

---

## Directory layout

```
vstbridge/
├── meson.build            # Top-level build definition
├── meson_options.txt      # Build options (bitbridge, clap, vst3, winedbg, system-asio)
├── cross-wine.conf        # Meson cross-file for winegcc-stable / wineg++-stable
├── src/
│   ├── common/            # Shared code: config, serialization, logging, IPC
│   │   └── config/        # config.h.in, version.h.in — Meson generates config.h, version.h
│   ├── chainloader/       # Tiny stub .so files that dlopen() the real plugin .so
│   ├── plugin/            # Native Linux .so files loaded by the DAW
│   └── wine-host/         # Wine (Windows) side: hosts the actual Windows plugin
│       └── editor.cpp     # Contains the Wine ≥9.21 XEmbed fix
├── subprojects/           # Meson wrap files for asio, bitsery, clap, function2, ghc_filesystem, tomlplusplus, vst3
└── tools/
    └── vstbridgectl/      # Management tool (C++20 + GTK3, Makefile-based)
        ├── Makefile
        ├── install.sh
        ├── vstbridgectl-gtk.desktop
        └── src/
            ├── config.h / config.cpp   # Config struct, XDG paths, VstbridgeFiles
            ├── main.cpp                # CLI entry point (CLI11)
            ├── gui_main.cpp            # GTK entry point
            ├── actions.cpp             # sync, status, set, add, rm operations
            ├── util.cpp                # Host binary detection, Wine version check
            ├── files.cpp               # Plugin file search/indexing
            ├── symbols.cpp             # ELF symbol inspection
            ├── vst3_moduleinfo.cpp     # VST3 bundle metadata
            └── gui/
                ├── app_window.h        # GTK widget struct
                └── app_window.cpp      # Full GTK3 UI (tabs: Directories, Sync, Status, Settings)
```

---

## Build system

**Language:** C++20  
**Build tool:** Meson + Ninja  
**Cross compiler:** `winegcc-stable` / `wineg++-stable` (configured in `cross-wine.conf`)  
**Native compiler:** g++ (system default)

### Build outputs (all in `build/`)

| File | Description |
|------|-------------|
| `libvstbridge-vst2.so` | Native .so — loaded by DAW for VST2 plugins |
| `libvstbridge-vst3.so` | Native .so — loaded by DAW for VST3 plugins |
| `libvstbridge-clap.so` | Native .so — loaded by DAW for CLAP plugins |
| `libvstbridge-chainloader-vst2.so` | Tiny stub for VST2 (the file actually copied per-plugin) |
| `libvstbridge-chainloader-vst3.so` | Tiny stub for VST3 |
| `libvstbridge-chainloader-clap.so` | Tiny stub for CLAP |
| `vstbridge-host.exe` | Wine (64-bit) plugin host process |
| `vstbridge-host-32.exe` | Wine (32-bit) plugin host process (requires `-Dbitbridge=true`) |

### Configure and build

```bash
cd vstbridge/

# Standard 64-bit build
meson setup build --buildtype=release --cross-file=cross-wine.conf
ninja -C build

# With CLAP + VST3 + 32-bit bitbridge
meson setup build --buildtype=release --cross-file=cross-wine.conf \
  -Dclap=true -Dvst3=true -Dbitbridge=true
ninja -C build
```

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `bitbridge` | false | Build 32-bit host for 32-bit Windows plugins |
| `clap` | true | Build CLAP support |
| `vst3` | true | Build VST3 support |
| `system-asio` | false | Use system `<asio.hpp>` instead of subproject wrap |
| `winedbg` | false | Run Wine host under GDB |

### 32-bit build prerequisites (this system)

The system uses `wine-staging` at `/opt/wine-staging/`. Debian's `wine32` package has `.dll` files but not the `.a` import libraries that `winegcc` needs. Three symlinks are required:

```bash
# 1. Point wine's i386-windows dir at staging's copy
sudo rm /usr/lib/wine/i386-windows
sudo ln -s /opt/wine-staging/lib/wine/i386-windows /usr/lib/wine/i386-windows

# 2. Create i386-unix dir that winegcc looks for
sudo mkdir -p /usr/lib/x86_64-linux-gnu/wine/wine
sudo ln -s /opt/wine-staging/lib/wine/i386-unix /usr/lib/x86_64-linux-gnu/wine/wine/i386-unix

# Required packages
sudo apt install g++-multilib libxcb1-dev:i386 uuid-dev:i386 wine32
```

---

## vstbridgectl tool

**Language:** C++20  
**Build:** Makefile (fetches deps with `git clone` on first build)  
**Dependencies:** CLI11, nlohmann/json, tomlplusplus (auto-fetched), GTK3 (system)

```bash
cd tools/vstbridgectl/

# Fetch deps and build both CLI and GUI binaries
make

# Install to ~/.local/bin (user) or /usr/local/bin (--system)
./install.sh
./install.sh --system
```

**Outputs:**
- `build/vstbridgectl` — CLI tool
- `build/vstbridgectl-gtk` — GTK3 GUI

**Config file:** `~/.config/vstbridgectl/config.toml`  
**Data/install dir:** `~/.local/share/vstbridge/` (where the built `.so` and `.exe` files are installed)

### GUI tabs
- **Directories** — manage plugin scan directories
- **Sync** — run sync with force/prune/verbose/no-verify options, live output log
- **Status** — show current vstbridge installation status
- **Settings** — vstbridge path, VST2 install mode (centralized/inline), Wine/vstbridge compatibility check toggle

---

## Key configuration

**Plugin config file:** `vstbridge.toml` (placed alongside plugin files or in a parent directory)

**Environment variables:**
- `VSTBRIDGE_DEBUG_FILE` — redirect log output to a file
- `VSTBRIDGE_DEBUG_LEVEL` — verbosity (0–3, default 0)
- `VSTBRIDGE_NO_WATCHDOG` — disable the watchdog timer
- `VSTBRIDGE_TEMP_DIR` — override the temp directory for sockets

**Socket naming:** `/run/user/<uid>/vstbridge-<plugin>-<random>/`  
**Group sockets:** `/tmp/vstbridge-group-<name>-<prefix_hash>-<arch>.sock`

---

## Critical architectural notes

### Chainloader/plugin ABI
The chainloader `.so` files use `dlopen()` + `dlsym()` to call into the main plugin `.so`. The `LOAD_FUNCTION` macro stringifies the C variable name, so variable names ARE the ABI. These exported symbol names must always match between chainloader and plugin:

| Chainloader calls (dlsym) | Plugin exports |
|---------------------------|----------------|
| `vstbridge_plugin_init` | VST2 bridge init |
| `vstbridge_module_init` | VST3/CLAP bridge init |
| `vstbridge_module_free` | VST3/CLAP bridge teardown |
| `vstbridge_module_get_plugin_factory` | VST3 factory |
| `vstbridge_module_get_factory` | CLAP factory |
| `vstbridge_version` | version string |

### The Wine ≥9.21 XEmbed fix
XEmbed is enabled by default (`editor_xembed = true` in `configuration.h`). The fix in `editor.cpp`:
- `fix_local_coordinates()` now calls `SetWindowPos()` in XEmbed mode instead of sending a `ConfigureNotify` (which was blocked by `wm_state_serial` never clearing for embedded windows)
- `fix_local_coordinates()` is also called from the `VisibilityNotify` handler after `do_xembed()`

See `readme-fixes.md` at the repo root for the full technical analysis.

---

## What has NOT been done yet

- **README.md** — the existing README is from upstream yabridge and references AUR packages, Arch Linux setup, etc. that do not apply to vstbridge. It needs to be replaced with vstbridge-specific documentation.
- **Installation packaging** — no distro packages exist yet; users must build from source.
- **The repo's git history** — files at `vstbridge/` are currently untracked (the tracked history is under the old `yabridge-5.1.1/` path). A clean commit of the renamed project has not been made yet.

---

## Upstream reference

Forked from: yabridge 5.1.1 by Robbert van der Helm  
Upstream: https://github.com/robbert-vdh/yabridge  
License: GPL-3.0
