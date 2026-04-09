/*
 * Copyright (C) 2026
 * VST3 Bridge - Wine VST3 Host Bridge
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/**
 * @file vst3_host.h
 * @brief Wine-side VST3 plugin DLL loader and factory holder.
 *
 * Responsible for:
 *  1. Loading the Windows VST3 plugin DLL with LoadLibraryW().
 *  2. Resolving the GetPluginFactory entry-point.
 *  3. Calling GetPluginFactory() and storing the resulting IPluginFactory*.
 *  4. Providing the factory to the rest of the Wine host for class
 *     enumeration and instance creation.
 *  5. Calling the optional ModuleInit / ModuleExit functions if present.
 *  6. Properly releasing the factory and unloading the DLL on shutdown.
 */

#pragma once

// Must be included before any other Windows headers to avoid conflicts.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "pluginterfaces/base/ipluginbase.h"

namespace vst3bridge {

/**
 * @brief Manages the lifetime of a loaded VST3 plugin DLL.
 *
 * Exactly one VST3Host instance is created per Wine host process.
 * The object must not be copied or moved.
 */
class VST3Host {
public:
    VST3Host() noexcept = default;
    ~VST3Host();

    VST3Host(const VST3Host&) = delete;
    VST3Host& operator=(const VST3Host&) = delete;
    VST3Host(VST3Host&&) = delete;
    VST3Host& operator=(VST3Host&&) = delete;

    // ---- Lifecycle ----------------------------------------------------------

    /**
     * @brief Load the plugin DLL and retrieve the plugin factory.
     *
     * Calls ModuleInit() if the DLL exports it.  The factory reference
     * count is incremented by GetPluginFactory() and owned by this object.
     *
     * @param path  Absolute Windows path to the .vst3 DLL or bundle entry
     *              (e.g. C:\\VSTPlugins\\MyPlugin.vst3\\Contents\\x86_64-win\\MyPlugin.vst3)
     * @return true on success, false if the DLL or entry-point is missing.
     */
    bool loadPlugin(const wchar_t* path);

    /**
     * @brief Release the factory and unload the DLL.
     *
     * Safe to call even if loadPlugin() was never called or failed.
     * Calls ModuleExit() if the DLL exports it.
     */
    void unloadPlugin();

    // ---- Accessors ----------------------------------------------------------

    /** @return The IPluginFactory, or nullptr if no plugin is loaded. */
    Steinberg::IPluginFactory* getFactory() const noexcept { return factory_; }

    /**
     * @brief Query for IPluginFactory2 (extended class info).
     *
     * The returned pointer is not ref-counted separately — release()
     * must not be called on it.  It is valid for the lifetime of this object.
     *
     * @return IPluginFactory2 pointer or nullptr.
     */
    Steinberg::IPluginFactory2* getFactory2() const noexcept { return factory2_; }

    /**
     * @brief Query for IPluginFactory3 (Unicode + host-context).
     * @return IPluginFactory3 pointer or nullptr.
     */
    Steinberg::IPluginFactory3* getFactory3() const noexcept { return factory3_; }

    /** @return true if a plugin DLL is currently loaded. */
    bool isLoaded() const noexcept { return module_ != nullptr; }

private:
    HMODULE                      module_   = nullptr;
    Steinberg::IPluginFactory*   factory_  = nullptr;
    Steinberg::IPluginFactory2*  factory2_ = nullptr;
    Steinberg::IPluginFactory3*  factory3_ = nullptr;

    // Optional module lifecycle symbols
    using ModuleInitProc = bool (PLUGIN_API*)();
    using ModuleExitProc = bool (PLUGIN_API*)();

    ModuleInitProc moduleInit_ = nullptr;
    ModuleExitProc moduleExit_ = nullptr;
};

} // namespace vst3bridge
