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
 * @file socket.h
 * @brief Unix domain socket wrapper
 */

#pragma once

#include "protocol.h"
#include <memory>
#include <string>
#include <chrono>

namespace vst3bridge {

/**
 * @brief Unix domain socket wrapper
 */
class MessageSocket {
public:
    /**
     * @brief Construct from existing file descriptor
     * @param fd Socket file descriptor
     */
    explicit MessageSocket(int fd);
    
    ~MessageSocket();

    // Non-copyable
    MessageSocket(const MessageSocket&) = delete;
    MessageSocket& operator=(const MessageSocket&) = delete;

    // Movable
    MessageSocket(MessageSocket&& other) noexcept;
    MessageSocket& operator=(MessageSocket&& other) noexcept;

    /**
     * @brief Send a complete message
     * @param type Message type
     * @param payload Payload data
     * @param payload_size Payload size in bytes
     * @return true on success
     */
    bool sendMessage(MsgType type, const void* payload, size_t payload_size);

    /**
     * @brief Receive a message header
     * @param header Output header
     * @return true on success
     */
    bool receiveHeader(MessageHeader& header);

    /**
     * @brief Receive message payload
     * @param buffer Buffer to receive into
     * @param size Expected size
     * @return true on success
     */
    bool receivePayload(void* buffer, size_t size);

    /**
     * @brief Receive complete message (header + payload)
     * @param buffer Buffer for payload
     * @param max_size Maximum buffer size
     * @param type Output: message type
     * @return true on success
     */
    bool receiveMessage(void* buffer, size_t max_size, MsgType& type);

    /**
     * @brief Receive message with timeout
     * @param buffer Buffer for payload
     * @param max_size Maximum buffer size
     * @param type Output: message type
     * @param timeout Timeout duration
     * @return true on success
     */
    bool receiveMessageWithTimeout(void* buffer, size_t max_size, 
                                   MsgType& type, 
                                   std::chrono::milliseconds timeout);

    /**
     * @brief Receive specific response type
     * @tparam ResponseType Expected response structure
     * @param response Output response
     * @return true on success
     */
    template<typename ResponseType>
    bool receiveMessage(ResponseType& response) {
        MsgType type;
        return receiveMessage(&response, sizeof(response), type);
    }

    /**
     * @brief Send raw bytes without a message header.
     *
     * Used for sending state-data blobs that follow a header message.
     * The caller must hold the socket mutex if shared across threads.
     *
     * @param data  Pointer to the bytes to send.
     * @param size  Number of bytes to send.
     * @return true on success.
     */
    bool sendRaw(const void* data, size_t size);

    /**
     * @brief Receive exactly @p size raw bytes without reading a header.
     *
     * Used for reading state-data blobs that follow a header message.
     *
     * @param buf   Output buffer (must be at least @p size bytes).
     * @param size  Exact number of bytes to read.
     * @return true on success.
     */
    bool receiveRaw(void* buf, size_t size);

    /**
     * @brief Close the socket
     */
    void close();

    /**
     * @brief Check if socket is valid
     * @return true if connected
     */
    bool isValid() const { return fd_ >= 0; }

    /**
     * @brief Set receive timeout
     * @param timeout_ms Timeout in milliseconds
     */
    void setReceiveTimeout(int timeout_ms);

    /**
     * @brief Get underlying file descriptor
     * @return File descriptor
     */
    int getFd() const { return fd_; }

private:
    int fd_ = -1;
    bool sendAll(const void* data, size_t size);
    bool receiveAll(void* data, size_t size);
};

/**
 * @brief Socket server for accepting connections
 */
class SocketServer {
public:
    SocketServer();
    ~SocketServer();

    // Non-copyable and non-movable
    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;
    SocketServer(SocketServer&&) = delete;
    SocketServer& operator=(SocketServer&&) = delete;

    /**
     * @brief Bind to a Unix socket path
     * @param path Socket path
     * @return true on success
     */
    bool bind(const std::string& path);

    /**
     * @brief Listen for connections
     * @param backlog Listen backlog
     * @return true on success
     */
    bool listen(int backlog = 5);

    /**
     * @brief Accept a connection
     * @return Connected socket, nullptr on error
     */
    std::unique_ptr<MessageSocket> accept();

    /**
     * @brief Accept with timeout
     * @param timeout_ms Timeout in milliseconds
     * @return Connected socket, nullptr on timeout/error
     */
    std::unique_ptr<MessageSocket> acceptWithTimeout(int timeout_ms);

    /**
     * @brief Close the server socket
     */
    void close();

    /**
     * @brief Get socket path
     * @return Socket path
     */
    const std::string& getPath() const { return path_; }

private:
#ifdef _WIN32
    SOCKET fd_ = INVALID_SOCKET;
#else
    int fd_ = -1;
#endif
    std::string path_;
};

/**
 * @brief Connect to a Unix socket
 * @param path Socket path
 * @return Connected socket, nullptr on error
 */
std::unique_ptr<MessageSocket> connectToSocket(const std::string& path);

/**
 * @brief Connect with timeout
 * @param path Socket path
 * @param timeout_ms Timeout in milliseconds
 * @return Connected socket, nullptr on timeout/error
 */
std::unique_ptr<MessageSocket> connectToSocketWithTimeout(const std::string& path, 
                                                          int timeout_ms);

} // namespace vst3bridge
