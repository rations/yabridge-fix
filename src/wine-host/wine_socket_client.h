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

#ifndef VST3BRIDGE_WINE_SOCKET_CLIENT_H
#define VST3BRIDGE_WINE_SOCKET_CLIENT_H

/**
 * @file wine_socket_client.h
 * @brief Wine-side Unix socket client for IPC with native plugin
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>

#include "protocol.h"

namespace vst3bridge {

/**
 * @brief Wine-side Unix socket client
 *
 * Wraps Winsock2 Unix domain socket (AF_UNIX) functionality for
 * communication with the native Linux plugin.
 *
 * Wine maps AF_UNIX to native Unix domain sockets, so this works
 * transparently across the Wine / native boundary.
 */
class WineSocketClient {
public:
    WineSocketClient() noexcept = default;

    ~WineSocketClient() { close(); }

    WineSocketClient(const WineSocketClient&) = delete;
    WineSocketClient& operator=(const WineSocketClient&) = delete;

    // ---- Lifecycle ----------------------------------------------------------

    /**
     * @brief Connect to Unix domain socket
     * @param path Socket path (typically in /tmp or runtime directory)
     * @return true on success
     */
    bool connect(const std::string& path);

    /** Close the socket and cleanup Winsock */
    void close();

    /** Check if socket is connected */
    bool isConnected() const noexcept { return socket_ != INVALID_SOCKET; }

    // ---- Send ---------------------------------------------------------------

    /**
     * @brief Send a complete framed message (header + payload).
     * @param type         Message type.
     * @param payload      Payload bytes (may be nullptr if size is 0).
     * @param payloadSize  Payload size in bytes.
     * @return true on success.
     */
    bool sendMessage(MsgType type,
                     const void* payload,
                     size_t payloadSize) noexcept;

    /**
     * @brief Send raw bytes after an already-sent header (used for state data).
     */
    bool sendRaw(const void* data, size_t size) noexcept;

    // ---- Receive ------------------------------------------------------------

    /**
     * @brief Receive one framed message into @p msg.
     * @param msg         Output: populated with header and payload bytes.
     * @param timeoutMs   Milliseconds to wait; 0 = block indefinitely.
     * @return true if a complete message was received.
     */
    bool receiveMessage(GenericMessage& msg, int timeoutMs = 0) noexcept;

    /**
     * @brief Receive exactly @p size raw bytes (e.g., state data after header).
     */
    bool recvRaw(void* buf, size_t size) noexcept;

private:
    bool sendAll(const void* data, size_t size) noexcept;
    bool recvAll(void* data, size_t size) noexcept;

    SOCKET socket_ = INVALID_SOCKET;
};

} // namespace vst3bridge

#endif // VST3BRIDGE_WINE_SOCKET_CLIENT_H