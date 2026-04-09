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
 * @file socket.cpp
 * @brief Unix domain socket implementation
 */

#ifdef BUILDING_WINE_HOST
// Wine host: use TCP sockets
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#else
// Native Linux
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#endif


#include "socket.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <chrono>

namespace vst3bridge {

// =============================================================================
// MessageSocket Implementation
// =============================================================================

MessageSocket::MessageSocket(int fd) : fd_(fd) {
    // Set socket to blocking mode by default
#ifdef _WIN32
    // On Windows, we don't need to set blocking mode explicitly for our usage
#else
    if (fd_ >= 0) {
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
        }
    }
#endif
}

MessageSocket::~MessageSocket() {
    close();
}

MessageSocket::MessageSocket(MessageSocket&& other) noexcept 
    : fd_(other.fd_) {
    other.fd_ = -1;
}

MessageSocket& MessageSocket::operator=(MessageSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

bool MessageSocket::sendAll(const void* data, size_t size) {
    if (fd_ < 0) return false;
    
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = size;
    
    while (remaining > 0) {
#ifdef _WIN32
        int sent = send(fd_, ptr, static_cast<int>(remaining), 0);
#else
        ssize_t sent = send(fd_, ptr, remaining, MSG_NOSIGNAL);
#endif
        if (sent < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

bool MessageSocket::receiveAll(void* data, size_t size) {
    if (fd_ < 0) return false;
    
    char* ptr = static_cast<char*>(data);
    size_t remaining = size;
    
    while (remaining > 0) {
#ifdef _WIN32
        int received = recv(fd_, ptr, static_cast<int>(remaining), 0);
#else
        ssize_t received = recv(fd_, ptr, remaining, 0);
#endif
        if (received <= 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        ptr += received;
        remaining -= received;
    }
    return true;
}

bool MessageSocket::sendMessage(MsgType type, const void* payload, size_t payload_size) {
    if (fd_ < 0) return false;
    
    MessageHeader header;
    header.magic = MessageHeader::kMagic;
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.payload_size = static_cast<uint32_t>(payload_size);
    header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    
    // Send header
    if (!sendAll(&header, sizeof(header))) {
        return false;
    }
    
    // Send payload if any
    if (payload_size > 0 && payload) {
        return sendAll(payload, payload_size);
    }
    
    return true;
}

bool MessageSocket::receiveHeader(MessageHeader& header) {
    if (!receiveAll(&header, sizeof(header))) {
        return false;
    }
    
    // Validate magic number
    if (header.magic != MessageHeader::kMagic) {
        return false;
    }
    
    // Validate protocol version
    if (header.version != PROTOCOL_VERSION) {
        return false;
    }
    
    return true;
}

bool MessageSocket::receivePayload(void* buffer, size_t size) {
    return receiveAll(buffer, size);
}

bool MessageSocket::receiveMessage(void* buffer, size_t max_size, MsgType& type) {
    MessageHeader header;
    if (!receiveHeader(header)) {
        return false;
    }
    
    type = header.type;
    
    if (header.payload_size > max_size) {
        // Payload too large - drain it
        char drain[4096];
        size_t remaining = header.payload_size;
        while (remaining > 0) {
            size_t to_read = std::min(remaining, sizeof(drain));
            if (!receiveAll(drain, to_read)) {
                return false;
            }
            remaining -= to_read;
        }
        return false;
    }
    
    if (header.payload_size > 0) {
        return receiveAll(buffer, header.payload_size);
    }
    
    return true;
}

bool MessageSocket::receiveMessageWithTimeout(void* buffer, size_t max_size, 
                                                MsgType& type, 
                                                std::chrono::milliseconds timeout) {
    if (fd_ < 0) return false;
    
#ifdef _WIN32
    // Windows implementation using select
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    
    timeval tv;
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    
    int ret = select(0, &fds, nullptr, nullptr, &tv);
    if (ret <= 0) {
        return false;  // Timeout or error
    }
#else
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    
    int ret = poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (ret <= 0) {
        return false;  // Timeout or error
    }
#endif
    
    return receiveMessage(buffer, max_size, type);
}

bool MessageSocket::sendRaw(const void* data, size_t size) {
    return sendAll(data, size);
}

bool MessageSocket::receiveRaw(void* buf, size_t size) {
    return receiveAll(buf, size);
}

void MessageSocket::close() {
    if (fd_ >= 0) {
#ifdef _WIN32
        closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }
}

void MessageSocket::setReceiveTimeout(int timeout_ms) {
    if (fd_ < 0) return;

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
}

// =============================================================================
// SocketServer Implementation
// =============================================================================

SocketServer::SocketServer() = default;

SocketServer::~SocketServer() {
    close();
}

bool SocketServer::bind(const std::string& path) {
#ifdef BUILDING_WINE_HOST
    int port = std::stoi(path);
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ == INVALID_SOCKET) {
        return false;
    }
    
    // Allow reuse of address
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(fd_);
        fd_ = -1;
        return false;
    }
    
    path_ = path;
    return true;
#else
    // Remove existing socket file
    remove(path.c_str());
    
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }
    
    // Allow reuse of address
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    
    path_ = path;
    return true;
#endif
}

bool SocketServer::listen(int backlog) {
    if (fd_ < 0) return false;
    return ::listen(fd_, backlog) == 0;
}

std::unique_ptr<MessageSocket> SocketServer::accept() {
    if (fd_ < 0) return nullptr;

#ifdef BUILDING_WINE_HOST
    sockaddr_in addr;
    int len = sizeof(addr);

    SOCKET client_fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (client_fd == INVALID_SOCKET) {
        return nullptr;
    }
    
    return std::make_unique<MessageSocket>(client_fd);
#else
    struct sockaddr_un addr;
    socklen_t len = sizeof(addr);
    
    int client_fd = ::accept(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    if (client_fd < 0) {
        return nullptr;
    }
    
    return std::make_unique<MessageSocket>(client_fd);
#endif
}

std::unique_ptr<MessageSocket> SocketServer::acceptWithTimeout(int timeout_ms) {
    if (fd_ < 0) return nullptr;

#ifdef _WIN32
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (ret <= 0) {
        return nullptr;  // Timeout or error
    }
#else
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        return nullptr;  // Timeout or error
    }
#endif

    return accept();
}

void SocketServer::close() {
    if (fd_ >= 0) {
#ifdef BUILDING_WINE_HOST
        closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }
#ifndef BUILDING_WINE_HOST
    if (!path_.empty()) {
        remove(path_.c_str());
        path_.clear();
    }
#endif
}

// =============================================================================
// Client Connection Functions
// =============================================================================

std::unique_ptr<MessageSocket> connectToSocket(const std::string& path) {
#ifdef BUILDING_WINE_HOST
    int port = std::stoi(path);
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        return nullptr;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(fd);
        return nullptr;
    }
    
    return std::make_unique<MessageSocket>(static_cast<int>(fd));
#else
    // Linux/Unix implementation
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return nullptr;
    }
    
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return nullptr;
    }
    
    return std::make_unique<MessageSocket>(fd);
#endif
}

std::unique_ptr<MessageSocket> connectToSocketWithTimeout(const std::string& path, 
                                                            int timeout_ms) {
#ifdef BUILDING_WINE_HOST
    int port = std::stoi(path);
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        return nullptr;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    // Set non-blocking mode
    u_long nonblocking_mode = 1; // 1 for non-blocking
    ioctlsocket(fd, FIONBIO, &nonblocking_mode);

    // Attempt connection
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(fd);
            return nullptr;
        }
        // Connection in progress - wait for it
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        
        int result = select(0, nullptr, &writefds, nullptr, &tv);
        if (result > 0) {
            // Check connection status
            int so_error = 0;
            int len = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
            if (so_error == 0) {
                // Connection succeeded
                // Set back to blocking mode
                u_long blocking_mode = 0;
                ioctlsocket(fd, FIONBIO, &blocking_mode);
                return std::make_unique<MessageSocket>(static_cast<int>(fd));
            }
        }
        // Connection failed or timed out
        closesocket(fd);
        return nullptr;
    }
    
    // Connection succeeded immediately
    // Set back to blocking mode
    u_long blocking_mode = 0; // 0 for blocking
    ioctlsocket(fd, FIONBIO, &blocking_mode);
    return std::make_unique<MessageSocket>(static_cast<int>(fd));
#else
    // Linux/Unix implementation
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return nullptr;
    }
    
    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    
    int result = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno == EINPROGRESS) {
        // Connection in progress - wait for it
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        
        result = poll(&pfd, 1, timeout_ms);
        if (result > 0) {
            int so_error;
            socklen_t len = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error == 0) {
                result = 0;  // Success
            } else {
                result = -1;
            }
        } else {
            result = -1;
        }
    }
    
    if (result < 0) {
        ::close(fd);
        return nullptr;
    }
    
    // Set back to blocking
    fcntl(fd, F_SETFL, flags);
    
    return std::make_unique<MessageSocket>(fd);
#endif
}

} // namespace vst3bridge
