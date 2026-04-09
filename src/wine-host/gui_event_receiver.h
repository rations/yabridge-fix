/*
 * Copyright (C) 2026
 * VST3 Bridge - Wine VST3 Host Bridge
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/**
 * @file gui_event_receiver.h
 * @brief GUI event receiver for Wine side
 */

#pragma once

#include "ipc_host.h"
#include "gui_events.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <memory>

namespace vst3bridge { class WineSocketClient; }
using namespace vst3bridge;

/**
 * @brief Receives GUI events from native side and forwards to plugin
 * 
 * This class runs in the Wine host process and receives forwarded
 * GUI events via IPC, then injects them into the plugin's window.
 */
class GUIEventReceiver {
public:
    /**
     * @brief Construct GUI event receiver
     * @param socket Socket for communication with native side
     * @param plugin_window Plugin window handle
     */
    GUIEventReceiver(vst3bridge::WineSocketClient* socket, HWND plugin_window);

    ~GUIEventReceiver();

    /**
     * @brief Start receiving events
     * @return true if started successfully
     */
    bool start();

    /**
     * @brief Stop receiving events
     */
    void stop();

    /**
     * @brief Check if receiver is running
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Update the plugin window handle (if plugin recreates window)
     * @param window New window handle
     */
    void setPluginWindow(HWND window) { plugin_window_ = window; }

private:
    /**
     * @brief Event receive loop
     */
    void receiveLoop();

    /**
     * @brief Process a received GUI event
     * @param event Event to process
     */
    void processEvent(const GUIEventMessage& event);

    /**
     * @brief Send mouse event to plugin window
     * @param event Mouse event
     */
    void sendMouseEvent(const MouseEvent& event, GUIEventType type);

    /**
     * @brief Send keyboard event to plugin window
     * @param event Key event
     */
    void sendKeyEvent(const KeyEvent& event, GUIEventType type);

    /**
     * @brief Convert MouseButton to Windows button flag
     */
    DWORD mouseButtonToFlag(MouseButton button) const;

    /**
     * @brief Convert ModifierKeys to Windows modifier flags
     */
    DWORD modifiersToFlags(const ModifierKeys& modifiers) const;

    vst3bridge::WineSocketClient* socket_;
    HWND plugin_window_;

    std::thread receive_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Current mouse position for wheel events
    int32_t last_mouse_x_ = 0;
    int32_t last_mouse_y_ = 0;
};

} // namespace vst3bridge
