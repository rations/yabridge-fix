# yabridge 5.1.1 Wine >= 9.21 Fix

## Problem

Windows VST2/VST3/CLAP plugin GUIs would not work under Wine >= 9.21 when bridged through yabridge. Plugins rendered correctly but were completely non-interactive — no mouse clicks, no knob movement, nothing.

## Root Cause

### Bug — Mouse input completely non-functional (wm_state_serial deadlock)

Even after fixing the reparent order, plugin GUIs rendered correctly but were completely non-interactive.

**The coordinate mismatch:** When a user clicks at screen position `(abs_x+dx, abs_y+dy)`, Wine routes the ButtonPress through `map_event_coords()` which uses the absolute root coordinates. These become the Win32 mouse event coordinates. Wine's server then calls `screen_to_client(hwnd, abs_x+dx, abs_y+dy)` to produce client-relative coordinates for the WM_LBUTTONDOWN lParam. If the HWND thinks it is at Win32 position `(0, 0)` — instead of the true screen position `(abs_x, abs_y)` — the resulting client coordinates are `(abs_x+dx, abs_y+dy)` which are completely outside the plugin's drawable area. The plugin ignores the click.

**Why `fix_local_coordinates()` didn't fix it:** `fix_local_coordinates()` sends a synthetic `ConfigureNotify` to `wine_window_` with the correct absolute coordinates. Wine's `X11DRV_ConfigureNotify` updates `current_state.rect` and posts `WM_WINE_WINDOW_STATE_CHANGED`. The Win32 event loop then calls `X11DRV_GetWindowStateUpdates` → `window_update_client_config` — but this function has an early exit:

```c
// window.c:1748
if (data->wm_state_serial) return 0;
```

When `make_window_embedded()` is called (from the XEMBED_EMBEDDED_NOTIFY handler), it calls `window_set_wm_state(data, NormalState, ...)` which sets `data->wm_state_serial = NextRequest(display)` and waits for a `WM_STATE` PropertyNotify from the window manager. For embedded windows inside a non-root parent, the WM never sees the window and never sets `WM_STATE`. So `wm_state_serial` stays non-zero for the entire lifetime of the plugin window, permanently blocking `window_update_client_config`. The Win32 HWND rect is never updated from `{0,0,w,h}` to `{abs_x,abs_y,abs_x+w,abs_y+h}`.

**The fix:** Since `editor.cpp` runs inside the Wine process, it can call Win32 APIs directly. Calling `SetWindowPos(win32_window_.handle_, nullptr, abs_x, abs_y, 0, 0, SWP_NOSIZE|...)` directly updates `data->rects.window` to the correct screen position, bypassing the `wm_state_serial` gating. Wine's embedded-window guard in `window_set_config` (`window.c:1417`) then prevents any actual `XConfigureWindow` call, so the X11 window stays visually correct at `(0,0)` relative to the wrapper window. Mouse coordinates are now translated correctly.

## Changes Made

### `src/wine-host/editor.cpp`

**1. Fixed `do_xembed()` — send EMBEDDED_NOTIFY before reparenting**

```cpp
// Before (broken):
do_reparent(wine_window_, wrapper_window_.window_);
send_xembed_message(wine_window_, xembed_embedded_notify_msg, ...);

// After (correct per XEmbed spec):
send_xembed_message(wine_window_, xembed_embedded_notify_msg, ...);
xcb_flush(x11_connection_.get());
do_reparent(wine_window_, wrapper_window_.window_);
```

**2. Fixed `fix_local_coordinates()` — use SetWindowPos in XEmbed mode**

In XEmbed mode, instead of sending a ConfigureNotify (which is blocked by `wm_state_serial`), call `SetWindowPos` directly to update the Win32 HWND screen position:

```cpp
if (use_xembed_) {
    SetWindowPos(win32_window_.handle_, nullptr, abs_x, abs_y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_NOREDRAW | SWP_NOSENDCHANGING);
    return;
}
```

**3. Added `fix_local_coordinates()` call in VisibilityNotify handler**

```cpp
if (use_xembed_) {
    do_xembed();
    fix_local_coordinates();  // added
}
```

**4. Removed the XEmbed early-return guard from `fix_local_coordinates()`**

The original code had `if (use_xembed_) { return; }` at the top of `fix_local_coordinates()`, preventing it from ever running in XEmbed mode. Removed.

### `src/common/configuration.h`

**Enabled XEmbed by default**

```cpp
// Before:
bool editor_xembed = false;

// After:
bool editor_xembed = true;
```

### `cross-wine.conf`

**Changed compiler to wine-stable**

```ini
c = 'winegcc-stable'
cpp = 'wineg++-stable'
```

Required because `/opt/wine-staging/include/` was empty on this system.

## Deployed Files

```
~/.local/share/yabridge/libyabridge-vst2.so
~/.local/share/yabridge/libyabridge-vst3.so
~/.local/share/yabridge/libyabridge-clap.so
~/.local/share/yabridge/yabridge-host.exe.so
~/.local/share/yabridge/yabridge-host.exe
```

The Wine-side host binary (`yabridge-host.exe`) is shared across all plugin formats. All three native `.so` files were redeployed because `configuration.h` is compiled into them.

## Key Wine Source References

- `dlls/winex11.drv/event.c:1456` — `handle_xembed_protocol` / `XEMBED_EMBEDDED_NOTIFY` handler, calls `make_window_embedded()`
- `dlls/winex11.drv/event.c:1061` — `X11DRV_ReparentNotify`, guarded by `data->embedded`
- `dlls/winex11.drv/window.c:1417` — `window_set_config` embedded guard, prevents XConfigureWindow position changes for embedded windows
- `dlls/winex11.drv/window.c:1622` — `window_set_wm_state` sets `wm_state_serial`
- `dlls/winex11.drv/window.c:1744` — `window_update_client_config` blocked by `wm_state_serial`
- `dlls/winex11.drv/mouse.c:501` — `map_event_coords` uses absolute root coordinates for ButtonPress
- `server/queue.c:1840` — `find_hardware_message_window` routes to visible `msg->win` directly
- `dlls/win32u/message.c:2727` — `screen_to_client` converts screen coords to client coords for WM_LBUTTONDOWN lParam
