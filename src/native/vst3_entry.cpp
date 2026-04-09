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
 * @file vst3_entry.cpp
 * @brief VST3 plugin entry point
 *
 * This is the entry point that gets loaded by the DAW. It starts the
 * Wine host process and returns a factory that proxies to the Windows plugin.
 */

#ifdef _WIN32
    #define SMTG_EXPORT_SYMBOL __declspec(dllexport)
#else
    #define SMTG_EXPORT_SYMBOL __attribute__ ((visibility ("default")))
#endif

#include "pluginterfaces/base/ipluginbase.h"
#include "socket.h"
#include "protocol.h"
#include "audio_shm.h"
#include "plugin_factory.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

namespace vst3bridge {

// Socket path for communication
static std::string g_socket_path;
static std::string g_shm_name;
static std::unique_ptr<PluginFactory> g_factory;

/**
 * @brief Get the path to the Wine host executable
 * @return Path to vst3bridge-host.exe
 */
static std::string getHostPath() {
    // Get the directory of this shared library
    std::string lib_path;
    
    // Try to find the host relative to this library
    // In a real implementation, this would use dladdr or similar
    // to find the library path at runtime
    
    const char* home = getenv("HOME");
    if (home) {
        lib_path = std::string(home) + "/.local/share/vst3bridge";
    }
    
    // Check if 32-bit plugin
    std::string host_name = "vst3bridge-host.exe";
    if (sizeof(void*) == 4) {
        host_name = "vst3bridge-host-32.exe";
    }
    
    return lib_path + "/" + host_name;
}

/**
 * @brief Get the path to the Windows VST3 plugin
 * @return Path to the .vst3 file in Wine prefix
 */
static std::string getPluginPath() {
    // The plugin path is passed via environment variable
    // This is set by the chainloader
    const char* path = getenv("VST3BRIDGE_PLUGIN_PATH");
    if (path) {
        return path;
    }
    
    // Fallback: try to derive from current working directory
    // or from a config file
    return "";
}

/**
 * @brief Get the Wine prefix to use
 * @return Path to Wine prefix
 */
static std::string getWinePrefix() {
    const char* prefix = getenv("WINEPREFIX");
    if (prefix) {
        return prefix;
    }
    
    const char* home = getenv("HOME");
    if (home) {
        return std::string(home) + "/.wine";
    }
    
    return "";
}

/**
 * @brief Start the Wine host process
 * @return true on success
 */
static bool startWineHost() {
    // Generate unique socket path
    g_socket_path = "/tmp/vst3bridge_" + std::to_string(getpid()) + "_" +
                    std::to_string(reinterpret_cast<uintptr_t>(&g_socket_path));
    
    // Generate unique shared memory name
    g_shm_name = "/vst3bridge_" + std::to_string(getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(&g_shm_name));
    
    // Initialize audio shared memory with reasonable defaults
    // TODO: Get actual values from plugin or configuration
    if (!initializeAudioSharedMemory(64, 8192)) {
        std::cerr << "Failed to initialize audio shared memory\n";
        return false;
    }
    
    // Create socket server
    SocketServer server;
    if (!server.bind(g_socket_path)) {
        return false;
    }
    
    if (!server.listen(1)) {
        return false;
    }
    
    // Fork and exec Wine host
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    
    if (pid == 0) {
        // Child process - exec Wine host
        
        // Set environment variables
        setenv("VST3BRIDGE_SOCKET_PATH", g_socket_path.c_str(), 1);
        setenv("VST3BRIDGE_PLUGIN_PATH", getPluginPath().c_str(), 1);
        setenv("VST3BRIDGE_SHM_NAME", g_shm_name.c_str(), 1);
        
        // Set up Wine environment
        std::string wine_prefix = getWinePrefix();
        if (!wine_prefix.empty()) {
            setenv("WINEPREFIX", wine_prefix.c_str(), 1);
        }
        
        // Unset DISPLAY to force Wine to use its own X11 handling
        // The host will create its own X11 connection
        
        // Execute Wine host
        std::string host_path = getHostPath();
        execlp("wine", "wine", host_path.c_str(), nullptr);
        
        // If we get here, exec failed
        _exit(1);
    }
    
    // Parent process - accept connection from Wine host
    auto socket = server.acceptWithTimeout(10000);  // 10 second timeout
    if (!socket) {
        // Kill the child process
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return false;
    }
    
    // Perform handshake
    MessageHeader header;
    if (!socket->receiveHeader(header)) {
        return false;
    }
    
    if (header.type != MsgType::Handshake) {
        return false;
    }
    
    // Send handshake response
    MsgResponseHandshake response;
    response.protocol_version = PROTOCOL_VERSION;
    
    if (!socket->sendMessage(MsgType::HandshakeResponse, 
                              &response, sizeof(response))) {
        return false;
    }
    
    // Create factory
    g_factory = std::make_unique<PluginFactory>(std::move(socket));
    
    return true;
}

/**
 * @brief Cleanup function
 */
static void cleanup() {
    g_factory.reset();
    
    // Clean up audio shared memory
    AudioBufferManager* audio_manager = getAudioBufferManager();
    if (audio_manager) {
        audio_manager->shutdown();
    }
    
    if (!g_socket_path.empty()) {
        unlink(g_socket_path.c_str());
    }
}

} // namespace vst3bridge

// =============================================================================
// VST3 Entry Point
// =============================================================================

extern "C" {

/**
 * @brief VST3 entry point
 * @return Plugin factory
 */
SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory* GetPluginFactory() {
    using namespace vst3bridge;
    
    // Register cleanup at exit
    static bool cleanup_registered = false;
    if (!cleanup_registered) {
        atexit(cleanup);
        cleanup_registered = true;
    }
    
    // If factory already exists, return it
    if (g_factory) {
        g_factory->addRef();
        return g_factory.get();
    }
    
    // Start Wine host and create factory
    if (!startWineHost()) {
        return nullptr;
    }
    
    g_factory->addRef();
    return g_factory.get();
}

/**
 * @brief Module init function (called on load)
 */
SMTG_EXPORT_SYMBOL bool InitDll() {
    return true;
}

/**
 * @brief Module exit function (called on unload)
 */
SMTG_EXPORT_SYMBOL bool ExitDll() {
    vst3bridge::cleanup();
    return true;
}

} // extern "C"

// Export marker for VST3 bundle
SMTG_EXPORT_SYMBOL Steinberg::uint32 GetPluginFactoryEntryPoint() {
    return 0;
}
