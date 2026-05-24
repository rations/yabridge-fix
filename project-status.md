# VST3 GDI Bridge — Project Status

## What This Is

A patch on yabridge 5.1.1 that makes Windows VST3 plugin GUIs work with Wine >= 9.21.
The original xcb_reparent_window embedding broke in that Wine version.

**Capture pipeline:** Plugin renders in hidden Win32 HWND inside `gdi_hide_container_`
(an X11 window at an offscreen Y position). XComposite REDIRECT_MANUAL captures frames
from the backing pixmap. A Wine-side thread reads them via xcb_get_image on a separate
XCB connection (`cap_conn`). Frames are written to a POSIX SHM ring buffer. The native
side reads from SHM and blits to `render_window_` via xcb_put_image. Input events
(click, drag, wheel) are written by native into a separate SPSC ring in SHM; the capture
thread reads them and dispatches as Win32 messages.

---

## What Works

- Single plugin: GUI displays, clicks and drags work
- Two plugins on the same track: both display and switch correctly
- Two plugins on different tracks: **both display** but Track 1 freezes initially

---

## The Two Bugs

### Bug 1 — Multi-track input freeze (main bug)

**Symptom:** Load Plugin A on Track 1, then Plugin B on Track 2. Clicking Plugin A's
dials does nothing. Close and reopen Track 1's FX window → both work.

**What diagnostics confirmed:**
- Click events ARE written to SHM and DO reach the Wine capture thread (PostMessage log fires)
- `GetForegroundWindow()` returns the correct wrapper HWND (0x10068) at LBD dispatch time
- `GetFocus()` returns the correct plugin child HWND (0x10070) at LBD dispatch time
- `GetActiveWindow()` returns the correct wrapper HWND at LBD dispatch time
- The guard (`gdi_activate_guard`) intercepts WM_LBUTTONDOWN and calls the original WndProc
- The plugin's original WndProc returns 0 (appears to handle it) but no dial movement occurs
- The capture and blit pipeline continues running normally during the freeze
- STALE does not fire during the freeze (plugin does not pause rendering to process input)

**Key observation:** After close+reopen, new HWNDs are assigned (0x20066/0x20070).
`GetForegroundWindow()` returns the new HWND directly without any AttachThreadInput
trick. Clicks work immediately. Something about fresh HWNDs is fundamentally different
from the old HWNDs after Track 2 loads.

**Root cause hypothesis:** When Track 2's `gdi_hide_container_` maps in X11, Wine's
x11drv sees stacking events and sends focus-related Win32 messages into Track 1's process.
Some internal Win32 or VSTGUI state in the plugin process changes such that the plugin
stops processing mouse input, even though all the Win32 API state (fgw, focus, active)
looks correct. The close+reopen works because `attached()` creates fresh Win32 windows and
the plugin's `IPlugView::attached()` reinitializes internal state (including VSTGUI's
`isActive_`, `pMouseDownView`, capture state, etc.).

**What the unique Y slot (off_y = my_slot * 32768) already does:**  
Each plugin's `gdi_hide_container_` is at a different Y position to avoid X11 stacking
events between containers. This was already deployed. It did NOT fix the freeze.

---

### Bug 2 — Mouse wheel never works

**Symptom:** Scrolling the mouse wheel over a plugin dial never moves it.

**What is happening:**
- Native sees wheel event (XCB scroll), writes type=8 to SHM
- Capture thread reads it, calls `SendMessage(WM_GDI_DO_WHEEL)` to main thread
- WM_GDI_DO_WHEEL handler: AttachThreadInput, SetForegroundWindow, SetFocus, then
  `SendMessage(plugin_window, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), screen_coords)`
- Plugin receives WM_MOUSEWHEEL and calls `GetPointerType()` (Wine stub, returns fail)
- No dial movement occurs (no STALE fires)

**Probable cause:** VSTGUI (and similar frameworks) use `GET_X_LPARAM(lParam)` /
`GET_Y_LPARAM(lParam)` from WM_MOUSEWHEEL to determine which view the cursor is over,
via `ScreenToClient()`. The `screen_coords` we send come from:
```
ClientToScreen(win32_window_.handle_, {input_ev.x, input_ev.y})
```
`win32_window_.handle_` (the Win32 wrapper) is at Win32 screen position (0, off_y) where
off_y = my_slot * 32768. But input_ev.x/y are coordinates relative to `render_window_`
on the native side, which is at a completely different screen position. The resulting
screen coordinates in lParam do NOT correspond to any view in the plugin's layout — so
`getViewAt()` returns null and the wheel event is silently dropped.

**The fix for wheel:** Compute the screen_coords differently. The plugin's Win32 window
is at screen position (0, off_y). The input coordinates are relative to render_window_
(native). We need to map them to the plugin's own client coordinate space. The correct
lParam for WM_MOUSEWHEEL should be the screen coordinates OF the plugin's Win32 window
plus the input offset — i.e., `MAKELPARAM(input_ev.x, off_y + input_ev.y)`. Or simpler:
don't use ClientToScreen at all; just pass `MAKELPARAM(input_ev.x, input_ev.y)` and let
VSTGUI's ScreenToClient do the math with (0, off_y) as the origin.

Actually the cleanest fix: send WM_MOUSEWHEEL with `client_coords` (already computed in
client space of plugin_window) as lParam, and set the flag `MK_CONTROL` or similar so
VSTGUI uses the provided coordinates directly rather than calling GetCursorPos. OR: use
`PostMessage` instead of `SendMessage` for WM_MOUSEWHEEL since some frameworks use
`GetCursorPos()` regardless of lParam.

---

## Architecture: Key Files

| File | Role |
|------|------|
| `src/wine-host/editor.h` | `Editor` class; `gdi_hide_container_`, `gdi_capture_thread_`, `gdi_cached_fg_tid_`, `gdi_pending_wheel_`, `gdi_plugin_subclassed_` |
| `src/wine-host/editor.cpp` | GDI init in `attached()`; capture thread; `window_proc` with WM_GDI_RESTORE_FOCUS / WM_GDI_DO_WHEEL handlers; `gdi_activate_guard` |
| `src/plugin/bridges/vst3-impls/plug-view-proxy.cpp` | Native side: SHM setup, `render_window_` creation, xcb_put_image blit loop, input event write |
| `src/common/frame_shm.h/.cpp` | SHM ring buffer for frames + input events |

---

## Key Code in editor.cpp

### gdi_activate_guard (~line 1543)
Subclassed WndProc on plugin_window (0x10070). Blocks `WM_ACTIVATE(WA_INACTIVE)` from
reaching VSTGUI so `isActive_` can't be set false. Logs fgw/focus/active at WM_LBUTTONDOWN.

### WM_GDI_RESTORE_FOCUS handler (~line 1632)
Triggered by capture thread (SendMessage) on every LBD. Does:
1. Subclass plugin_window with guard (first click only)
2. Send WM_ACTIVATE(WA_CLICKACTIVE) to wrapper and plugin_window
3. AttachThreadInput to foreground thread (cached if fgw=0)
4. SetForegroundWindow(wrapper), SetActiveWindow(wrapper), SetFocus(plugin_window)
5. SendMessage(plugin_window, WM_LBUTTONDOWN, MK_LBUTTON, client_coords)

### WM_GDI_DO_WHEEL handler (~line 1697)
Same focus-restoration path as WM_GDI_RESTORE_FOCUS but dispatches WM_MOUSEWHEEL.
Coordinates come from `gdi_pending_wheel_` staging struct (race-free via SendMessage block).

---

## Fixes Tried (All Failed or Incomplete)

| Fix | Outcome |
|-----|---------|
| SW_SHOWNOACTIVATE in show() | No effect on freeze |
| Intercept onFocus(false), always return true in native | No effect |
| SetForegroundWindow before PostMessage | No effect; caused crash on FX close |
| SendMessage instead of PostMessage for LBD | Freeze still present |
| WM_ACTIVATE nudges from STALE handler | No effect |
| Unique Y positions for gdi_hide_container_ (my_slot * 32768) | Deployed; did NOT fix freeze |
| gdi_activate_guard blocking WA_INACTIVE on plugin_window | Prevents isActive_ flip but freeze remains |
| WM_ACTIVATE(WA_CLICKACTIVE) injected into plugin_window on every click | Deployed; freeze remains |
| AttachThreadInput + SetForegroundWindow on every click | fgw correctly 0x10068 at LBD; freeze remains |
| Cached fg_tid when fgw=0 (gdi_cached_fg_tid_) | fgw stable across clicks; freeze remains |
| WM_GDI_DO_WHEEL via SendMessage to main thread | Wheel events reach plugin; no dial movement |
| subclass_child_with_guard on all children via EnumChildWindows | Deployed; freeze remains |

---

## Fixes To Try (Not Yet Attempted)

### For the freeze:

**A. Simulate close+reopen programmatically**  
Call `IPlugView::removed()` then `IPlugView::attached()` on Track 1 when Track 2 loads.
This recreates the Win32 windows with fresh HWNDs, which is exactly what the manual
close+reopen workaround does. The trigger could be a new message from the native side
to the Wine process when it detects a second plugin has started.

Risk: the `attached()`/`removed()` calls go through the plugin bridge serialization
stack — they may not be safe to call outside normal plugin lifecycle.

**B. Destroy and recreate win32_window_ on Track 2 load without removing the plugin**  
A lighter version of A: teardown only the Win32 wrapper window and recreate it (new HWND),
then re-embed the plugin_window as a child of the new wrapper. This avoids going through
the full plugin serialization stack.

**C. Re-investigate with more diagnostics**  
Add logging inside WM_GDI_DO_WHEEL (currently silent) and add WM_MOUSEWHEEL / WM_KILLFOCUS
logging to gdi_activate_guard. This would confirm:
- Whether WM_MOUSEWHEEL is actually reaching plugin_window (0x10070) after Amped loads
- Whether WM_KILLFOCUS is sent to plugin_window when Track 2 maps its container
- What fgw/focus/active look like at the moment WM_MOUSEWHEEL arrives

**D. Send full activation sequence before every click/wheel**  
Before each input event, send the full Windows activation sequence to plugin_window:
`WM_NCACTIVATE(TRUE)` → `WM_ACTIVATE(WA_CLICKACTIVE)` → `WM_SETFOCUS`
This covers frameworks that use WM_NCACTIVATE (not just WM_ACTIVATE) to track activation.

**E. Use INPUT_ONLY for gdi_hide_container_**  
Create gdi_hide_container_ as an INPUT_ONLY X11 window (no drawable surface). Requires
a different capture mechanism but would prevent x11drv from ever sending focus-change
messages based on X11 stacking, since INPUT_ONLY windows don't participate in the
compositor's stacking the same way.

### For the wheel:

**F. Fix screen_coords computation**  
`screen_coords` is computed as `ClientToScreen(win32_window_.handle_, pt)`. The wrapper
is at Win32 screen position (0, off_y). `input_ev.x, input_ev.y` are coordinates relative
to render_window_ on the native side, not relative to the wrapper. The resulting lParam
for WM_MOUSEWHEEL points to a position the plugin doesn't render anything at.

Fix: instead of lParam with wrong screen coords, pass `MAKELPARAM(input_ev.x, input_ev.y)`
directly in WM_MOUSEWHEEL lParam (treating plugin_window as if it were at (0,0) on the
virtual screen), OR pass the client_coords (already in plugin_window client space) and
modify VSTGUI's coordinate lookup. The simplest fix is probably to just skip the
ClientToScreen step and use the raw input_ev coordinates directly.

**G. Use WM_MOUSEMOVE before wheel to position VSTGUI's cursor tracking**  
Some frameworks cache the last WM_MOUSEMOVE position and use that for wheel hit testing
rather than reading lParam of WM_MOUSEWHEEL. Send a WM_MOUSEMOVE to plugin_window at the
wheel position immediately before sending WM_MOUSEWHEEL.

---

## Build & Deploy

```bash
cd /home/human/vst3bridge/yabridge-5.1.1
ninja -C build-mod 2>&1 | tail -10

# Wine side (editor changes):
cp build-mod/yabridge-host.exe.so ~/.local/share/yabridge/
cp build-mod/yabridge-host.exe ~/.local/share/yabridge/

# Native side (plug-view-proxy changes):
cp build-mod/libyabridge-vst3.so ~/.local/share/yabridge/

# Force rebuild:
touch src/wine-host/editor.cpp src/wine-host/editor.h
ninja -C build-mod
```

## Revert

```bash
cd /home/human/vst3bridge/yabridge-5.1.1
git checkout src/wine-host/editor.cpp src/wine-host/editor.h
ninja -C build-mod 2>&1 | tail -5
cp build-mod/yabridge-host.exe.so ~/.local/share/yabridge/
cp build-mod/yabridge-host.exe ~/.local/share/yabridge/
```
