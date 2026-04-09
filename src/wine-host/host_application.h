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
 * @file host_application.h
 * @brief Minimal IHostApplication implementation for the Wine host.
 *
 * This object is passed to IPluginBase::initialize() so the plugin can
 * query the host name and optionally request host-provided objects.
 * The bridge returns only the name; createInstance() returns
 * kNotImplemented for all requests.
 */

#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ihostapplication.h"

namespace vst3bridge {

/**
 * @brief Concrete IHostApplication for the VST3 Bridge Wine host.
 */
class HostApplication final : public Steinberg::IHostApplication {
public:
    HostApplication();

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override;
    Steinberg::uint32 PLUGIN_API release() override;

    // ---- IHostApplication --------------------------------------------------

    Steinberg::tresult PLUGIN_API getName(Steinberg::String128 name) override;

    Steinberg::tresult PLUGIN_API createInstance(
            Steinberg::TUID  cid,
            Steinberg::TUID  _iid,
            void**           obj) override;

    // ---- FUnknown iid ------------------------------------------------------

    static const Steinberg::FUID iid;

private:
    Steinberg::int32 refCount_;
};

} // namespace vst3bridge
