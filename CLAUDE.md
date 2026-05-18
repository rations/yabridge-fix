# VST3 GDI Capture Bridge — Project State

## What This Is

A patch on top of **yabridge 5.1.1** (`/home/human/vst3bridge/yabridge-5.1.1/`) that
makes Windows VST3 plugin GUIs work with Wine >= 9.21. The original yabridge
window-embedding approach (`xcb_reparent_window`) broke in that Wine version.

**The fix strategy:** GDI capture — the plugin renders inside a hidden Win32
HWND; a capture thread reads frames via BitBlt into a POSIX SHM ring buffer;
the native Linux side reads those frames and blits them into an X11 child window
that lives under the DAW's panel.

**Build directory:** `yabridge-5.1.1/build-mod/`  
**Deploy targets:** `~/.local/share/yabridge/libyabridge-vst3.so` and
`~/.local/share/yabridge/yabridge-host.exe.so` + `yabridge-host.exe`

---

## Files We Added / Changed

| File | What changed |
|------|-------------|
| `src/common/frame_shm.h/.cpp` | POSIX SHM triple-buffer ring + input event SPSC ring |
| `src/wine-host/gdi_capture.h/.cpp` | BitBlt GDI capture into 32bpp BGRA buffer |
| `src/wine-host/editor.h/.cpp` | GDI mode branch: hidden container, SHM open, Win32Thread capture loop, ChildWindowFromPointEx mouse routing |
| `src/wine-host/bridges/vst3.cpp` | Passes `frame_shm_name` to Editor constructor |
| `src/common/serialization/vst3/plug-view/plug-view.h` | `frame_shm_name` field in `Attached` struct |
| `src/plugin/bridges/vst3-impls/plug-view-proxy.h/.cpp` | Creates SHM, XCB connection, render window, GC; starts render thread |

---

## What Works (Single Plugin)

- Plugin GUI displays correctly inside Reaper's FX window
- Clicks, knob drags, button presses work
- Mouse wheel works on some plugins
- Presets work on some plugins (ML Sound Lab yes, Nembrini Audio inconsistent)

---

## What Is Broken (Multi-Plugin)

### Reaper FX Window Architecture (important context)

- Each **track** has its own FX window
- Within one track's FX window, all plugins in the chain share the **same parent
  XID** (the display panel). The user switches between them via a list on the
  side — Reaper shows/hides the panels, it does NOT call `removed()`/`attached()`
  on every switch.
- Different tracks have **different** parent XIDs.

### Symptoms

**Same track, two plugins:**
- Plugin 1 loads and works
- Plugin 2 loads — shows **black** in the FX window, or shows content mixed
  with whatever is behind the window
- Both `render_window_` instances are children of the same parent XID at (0,0),
  stacked on top of each other — only the topmost one (plugin 2, created last)
  is visible

**Different tracks, two plugins:**
- Plugin 1 on track 1 works
- Plugin 2 on track 2 loads and works  
- Plugin 1 **freezes** — GUI stops updating, mouse interaction stops
- Removing plugin 2 does **not** unfreeze plugin 1

**Confirmed working:** The Wine-side GDI capture is still running even when the
plugin appears frozen — briefly removing the frozen plugin shows
`gdi_hide_container_` (the hidden off-screen Wine window) at the top-left of
screen with the plugin rendering correctly inside it.

---

## Root Cause Hypothesis

The render thread on the native side (`plug-view-proxy.cpp`) blits frames via
`xcb_put_image` to `render_window_`. If `render_window_` is destroyed or its
parent is destroyed (Reaper may restructure its X11 window layout when a second
FX window opens), all subsequent `xcb_put_image` calls silently fail with
`BadDrawable`. The render thread keeps looping but nothing is displayed. This
is permanent — the thread never recovers, it just spins writing to a dead window.

The XCB connection itself may also be marked as failed after repeated protocol
errors, in which case `xcb_poll_for_event`, `xcb_put_image`, and `xcb_flush`
all become no-ops and the thread spins at 100% CPU.

---

## Steps Completed (Do Not Redo)

1. ✅ Replaced Xlib (`XOpenDisplay`) in render thread with XCB — fixed Xlib
   global state race when two render threads start simultaneously
2. ✅ Replaced `WindowFromPoint` with `ChildWindowFromPointEx` in the Wine
   capture thread — removed cross-process `WM_NCHITTEST` SendMessage
   (these fixed the symptoms that were present before; the multi-plugin freeze
   is a separate issue not yet addressed)

---

## Step-by-Step Fix Plan

### STEP 1 — Render thread: detect dead window/connection and reconnect

**Theory:** When Reaper opens a second FX window, it may restructure its X11
window hierarchy in a way that destroys `render_window_`'s parent (or the
render_window_ itself). The render thread needs to detect this and recreate the
window.

**Change:** Store `render_parent_xid_` as a member. In the render thread, after
`xcb_flush`, check `xcb_connection_has_error`. If the connection is dead,
close it, open a new XCB connection, recreate `render_window_` under
`render_parent_xid_`, recreate `render_gc_`, and continue rendering.

**Test:** Load plugin 1 on track 1, load plugin 2 on track 2. Does plugin 1
stay live after plugin 2 loads? Does plugin 1 recover if it briefly freezes?

**Expected result if hypothesis is correct:** Both plugins display and remain
interactive simultaneously on different tracks.

---

### STEP 2 — Same-track: Z-order / visibility management

**Theory:** Two plugins on the same track get the same `parent_xid`. Both
`render_window_` instances are at (0,0), stacked. Only the top one is visible.

**Change options:**
- Option A: Only show `render_window_` for the currently focused plugin.
  Listen to `IPlugView::onFocus(true/false)` to map/unmap `render_window_`.
- Option B: When `attached()` is called, raise our window to the top of the
  stacking order with `xcb_configure_window(..., XCB_STACK_MODE_ABOVE, ...)`.
  Use `onFocus` to re-raise when the user switches plugins.

**Test:** Load two plugins on the same track, switch between them in the FX
list. Does each plugin display correctly when selected?

---

### STEP 3 — Mouse wheel on dials

**Theory:** `WM_MOUSEWHEEL` is being posted but some plugins (Nembrini) do not
respond. May need `WM_MOUSEHWHEEL` or different target routing.

**Defer until steps 1 and 2 are resolved.**

---

### STEP 4 — Preset navigation (Nembrini Audio)

May be a Wine prefix issue (plugin not finding its preset directory) rather than
a GUI routing issue. Test with `WINEPREFIX` set correctly and check if
`~/.wine/drive_c/Users/$USER/Documents/` contains the plugin's preset folder.

**Defer until steps 1 and 2 are resolved.**

---

## Build & Deploy Commands

```bash
cd /home/human/vst3bridge/yabridge-5.1.1
ninja -C build-mod 2>&1 | tail -10

# Deploy native side (plug-view-proxy changes):
cp build-mod/libyabridge-vst3.so ~/.local/share/yabridge/

# Deploy Wine side (editor changes):
cp build-mod/yabridge-host.exe.so ~/.local/share/yabridge/
cp build-mod/yabridge-host.exe ~/.local/share/yabridge/
```

---

## Known Non-Issues

- The `gdi_hide_container_` briefly appearing at top-left when a plugin is
  removed is expected — it's Wine repositioning the HWND as a top-level window
  for ~1 second before `DeferredWin32Window` posts `WM_CLOSE`.
- Nembrini preset paths may require the Windows VST3 preset directory to exist
  under the Wine prefix.
