# vstbridge

vstbridge is a fork of [yabridge](https://github.com/robbert-vdh/yabridge) by Robbert van der Helm — a Wine-based bridge that allows Linux DAWs to load Windows VST2, VST3, and CLAP plugins as if they were native plugins.

Yabridge is an excellent project and this fork would not exist without it. All credit for the core bridging architecture, the IPC design, the chainloader system, and the VST3/CLAP compatibility work belongs to Robbert and the yabridge contributors. If you are on a distro with yabridge packages (Arch, Manjaro, etc.) and do not need the fixes below, use upstream yabridge.

---

## What vstbridge adds

### Wine ≥ 9.21 plugin GUI fix

Upstream yabridge 5.1.1 breaks with Wine ≥ 9.21: plugins render their GUI but are completely non-interactive. Knobs, buttons, and sliders do not respond to mouse input at all.

**Root cause:** When a plugin window is embedded via XEmbed, Wine's `make_window_embedded()` sets an internal `wm_state_serial` field and waits for a `WM_STATE` PropertyNotify from the window manager. For embedded (non-root) windows the window manager never sees the window, so `wm_state_serial` is never cleared. This permanently blocks `window_update_client_config` — the function that updates the Win32 HWND's screen position. The HWND stays at `{0, 0, w, h}` instead of its true screen position `{abs_x, abs_y, abs_x+w, abs_y+h}`. When the user clicks at `(abs_x+dx, abs_y+dy)`, Wine translates it to client coordinates relative to `(0, 0)`, producing `(abs_x+dx, abs_y+dy)` — coordinates outside the plugin window — so the click is discarded.

**The fix** (`src/wine-host/editor.cpp`): In XEmbed mode, `fix_local_coordinates()` calls `SetWindowPos()` directly instead of sending a `ConfigureNotify` event. `SetWindowPos()` updates the Win32 HWND rect without going through the `wm_state_serial`-gated path. Wine's embedded-window guard in `window_set_config` then prevents the actual X11 window from being moved visually, so the window stays correctly positioned on screen. Mouse coordinates are translated correctly and input works.

XEmbed is also enabled by default in this fork (`editor_xembed = true` in `src/common/configuration.h`).

For the full technical analysis see [readme-fixes.md](readme-fixes.md).

### GTK3 GUI for vstbridgectl

The upstream yabridgectl management tool is command-line only. vstbridge adds a GTK3 graphical interface (`vstbridgectl-gtk`) for users who prefer not to use the terminal.

The GUI has four tabs:

- **Directories** — add and remove the directories vstbridge scans for Windows plugins
- **Sync** — run a sync with options for force, prune, verbose output, and skipping the compatibility check; live output is shown in the window
- **Status** — show the current vstbridge installation status and detected Wine version
- **Settings** — set the vstbridge installation path, choose between centralized and inline VST2 install modes, and toggle the Wine/vstbridge compatibility check

---

## Requirements

- Wine Staging. This fork is tested with Wine Staging 11.09.
- Winetricks for DXVK
- A 64-bit Linux DAW (REAPER, Ardour etc.)
- GTK3 (for the vstbridgectl-gtk GUI only)

---

## Building from source

vstbridge uses Meson + Ninja and cross-compiles the Wine-side host binary with `winegcc-stable`/`wineg++-stable`.

```bash
git clone https://github.com/rations/vstbridge
cd vstbridge

# Configure
meson setup build --buildtype=release --cross-file=cross-wine.conf

# Build
ninja -C build
```

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `bitbridge` | false | Build a 32-bit host for 32-bit Windows plugins |
| `clap` | true | Build CLAP plugin support |
| `vst3` | true | Build VST3 plugin support |
| `system-asio` | false | Use the system `<asio.hpp>` instead of the bundled subproject |
| `winedbg` | false | Run the Wine host under winedbg |

Example with all plugin formats and 32-bit support:

```bash
meson setup build --buildtype=release --cross-file=cross-wine.conf \
  -Dclap=true -Dvst3=true -Dbitbridge=true
ninja -C build
```

### Build outputs

| File | Description |
|------|-------------|
| `libvstbridge-vst2.so` | Native .so loaded by the DAW for VST2 plugins |
| `libvstbridge-vst3.so` | Native .so loaded by the DAW for VST3 plugins |
| `libvstbridge-clap.so` | Native .so loaded by the DAW for CLAP plugins |
| `libvstbridge-chainloader-vst2.so` | Tiny stub copied per-plugin (VST2) |
| `libvstbridge-chainloader-vst3.so` | Tiny stub copied per-plugin (VST3) |
| `libvstbridge-chainloader-clap.so` | Tiny stub copied per-plugin (CLAP) |
| `vstbridge-host.exe` | Wine 64-bit plugin host process |
| `vstbridge-host-32.exe` | Wine 32-bit plugin host process (requires `bitbridge=true`) |

### Building vstbridgectl

```bash
cd tools/vstbridgectl

# Fetch dependencies and build both CLI and GUI
make

# Install to ~/.local/bin (user install)
./install.sh

# Install to /usr/local/bin (system install)
./install.sh --system
```

---

## Installation

After building, copy the build outputs to vstbridge's data directory and run a sync:

```bash
mkdir -p ~/.local/share/vstbridge
cp build/libvstbridge-{vst2,vst3,clap}.so \
   build/libvstbridge-chainloader-{vst2,vst3,clap}.so \
   build/vstbridge-host.exe \
   build/vstbridge-host.exe.so \
   ~/.local/share/vstbridge/

# Then use vstbridgectl to add your plugin directories and sync
vstbridgectl add ~/.wine/drive_c/Program\ Files/VstPlugins
vstbridgectl sync
```

Or use the GUI:

```bash
vstbridgectl-gtk
```

---

## Configuration

Plugin-level configuration is done via a `vstbridge.toml` file placed alongside plugin files or in a parent directory. See the upstream [yabridge configuration documentation](https://github.com/robbert-vdh/yabridge#configuration) for the available options — the TOML structure is unchanged.

### Environment variables

| Variable | Description |
|----------|-------------|
| `VSTBRIDGE_DEBUG_FILE` | Redirect log output to a file |
| `VSTBRIDGE_DEBUG_LEVEL` | Log verbosity: 0 (default) to 3 |
| `VSTBRIDGE_NO_WATCHDOG` | Disable the watchdog timer |
| `VSTBRIDGE_TEMP_DIR` | Override the temp directory used for sockets |

---

## License

GPL-3.0 — same as upstream yabridge. Copyright in individual source files is retained by the original author, Robbert van der Helm.
