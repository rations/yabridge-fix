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
 * @file plugin_factory.h
 * @brief Native-side VST3 factory proxy
 */

#pragma once

#include "pluginterfaces/base/ipluginbase.h"
#include "socket.h"
#include "protocol.h"
#include "plugin_proxy.h"
#include <atomic>
#include <memory>

namespace vst3bridge {

/**
 * @brief Plugin factory proxy
 * 
 * Implements the VST3 IPluginFactory, IPluginFactory2, and IPluginFactory3
 * interfaces, proxying calls to the Wine host via IPC.
 */
class PluginFactory : public Steinberg::IPluginFactory3 {
public:
    /**
     * @brief Construct factory proxy
     * @param socket Communication socket to Wine host
     */
    explicit PluginFactory(std::shared_ptr<MessageSocket> socket);
    
    virtual ~PluginFactory();

    // IPluginFactory implementation
    Steinberg::tresult PLUGIN_API getFactoryInfo(Steinberg::PFactoryInfo* info) override;
    Steinberg::int32 PLUGIN_API countClasses() override;
    Steinberg::tresult PLUGIN_API getClassInfo(Steinberg::int32 index, 
                                               Steinberg::PClassInfo* info) override;
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, 
                                                 Steinberg::TUID _iid, 
                                                 void** obj) override;

    // IPluginFactory2 implementation
    Steinberg::tresult PLUGIN_API getClassInfo2(Steinberg::int32 index, 
                                                Steinberg::PClassInfo2* info) override;

    // IPluginFactory3 implementation
    Steinberg::tresult PLUGIN_API getClassInfoUnicode(Steinberg::int32 index, 
                                                      Steinberg::PClassInfoW* info) override;
    Steinberg::tresult PLUGIN_API setHostContext(Steinberg::FUnknown* context) override;

    // FUnknown implementation
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, 
                                                 void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override;
    Steinberg::uint32 PLUGIN_API release() override;

private:
    std::shared_ptr<MessageSocket> socket_;
    std::atomic<Steinberg::uint32> ref_count_{1};
    Steinberg::FUnknown* host_context_ = nullptr;
};

} // namespace vst3bridge
