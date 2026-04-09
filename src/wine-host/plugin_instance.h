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
 * @file plugin_instance.h
 * @brief Lifecycle manager for one VST3 plugin instance (Wine side).
 *
 * PluginInstance wraps the raw COM objects returned by
 * IPluginFactory::createInstance() into a convenient, type-safe class.
 *
 * Ownership model:
 *  - The component_ and controller_ pointers are obtained via
 *    factory->createInstance() and QueryInterface respectively.
 *  - audioProc_ and view_ are obtained via QueryInterface on the component
 *    or factory.
 *  - All interfaces are released on destruction in the correct order.
 *  - componentHandler_ is the bridge-side IComponentHandler that the plugin
 *    calls back on GUI parameter changes.  It is owned by the caller
 *    (WineHost) and must outlive this object.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "protocol.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace vst3bridge {

/**
 * @brief Manages one VST3 plugin component+controller pair on the Wine side.
 *
 * Lifecycle:
 *   create() → initialize() → [use] → terminate() → destroy
 */
class PluginInstance {
public:
    /**
     * @brief Factory method: create and optionally initialise a plugin.
     *
     * @param factory      Loaded plugin factory.
     * @param cid          Class ID to instantiate (typically an IComponent).
     * @param hostApp      IHostApplication passed to initialize().
     *
     * @return Owning pointer to the new instance, or nullptr on failure.
     */
    static std::unique_ptr<PluginInstance> create(
            Steinberg::IPluginFactory*  factory,
            const Steinberg::TUID       cid,
            Steinberg::IHostApplication* hostApp);

    ~PluginInstance();

    // Non-copyable, non-movable
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;
    PluginInstance(PluginInstance&&) = delete;
    PluginInstance& operator=(PluginInstance&&) = delete;

    // ---- Identification -----------------------------------------------------

    /** Opaque instance ID used in protocol messages. */
    uint64_t getId() const noexcept { return id_; }

    // ---- IPluginBase --------------------------------------------------------

    /**
     * @brief Call initialize() on both component and controller.
     * @param hostApp  IHostApplication to pass.
     * @return kResultOk if both succeed.
     */
    Steinberg::tresult initialize(Steinberg::IHostApplication* hostApp);

    /**
     * @brief Call terminate() on both controller and component.
     * @return kResultOk if both succeed (or not applicable).
     */
    Steinberg::tresult terminate();

    // ---- IComponent ---------------------------------------------------------

    Steinberg::tresult getControllerClassId(Steinberg::FUID& classId);
    Steinberg::tresult setIoMode(Steinberg::IoMode mode);
    Steinberg::int32   getBusCount(Steinberg::MediaType type, Steinberg::BusDirection dir);
    Steinberg::tresult getBusInfo(Steinberg::MediaType type, Steinberg::BusDirection dir,
                                  Steinberg::int32 index, Steinberg::BusInfo& bus);
    Steinberg::tresult activateBus(Steinberg::MediaType type, Steinberg::BusDirection dir,
                                   Steinberg::int32 index, bool state);
    Steinberg::tresult setActive(bool state);

    /** Set component state from a raw byte buffer (from IPC). */
    Steinberg::tresult setComponentState(const std::vector<uint8_t>& data);

    /** Serialise component state to a byte buffer (for IPC). */
    Steinberg::tresult getComponentState(std::vector<uint8_t>& data);

    /** Set processor state from IPC bytes. */
    Steinberg::tresult setState(const std::vector<uint8_t>& data);

    /** Serialise processor state to bytes. */
    Steinberg::tresult getState(std::vector<uint8_t>& data);

    // ---- IAudioProcessor ----------------------------------------------------

    Steinberg::tresult setBusArrangements(const MsgRequestSetBusArrangements& msg);
    Steinberg::tresult getBusArrangement(Steinberg::BusDirection dir,
                                         Steinberg::int32 busIndex,
                                         Steinberg::SpeakerArrangement& arr);
    Steinberg::tresult canProcessSampleSize(Steinberg::int32 symbolicSize);
    Steinberg::uint32  getLatencySamples();
    Steinberg::tresult setupProcessing(Steinberg::ProcessSetup& setup);
    Steinberg::tresult setProcessing(bool state);
    Steinberg::tresult process(Steinberg::ProcessData& data);
    Steinberg::uint32  getTailSamples();

    // ---- IEditController ----------------------------------------------------

    Steinberg::tresult setControllerComponentState(const std::vector<uint8_t>& data);
    Steinberg::int32   getParameterCount();
    Steinberg::tresult getParameterInfo(Steinberg::int32 index, Steinberg::ParameterInfo& info);
    double             getParamNormalized(Steinberg::uint32 id);
    Steinberg::tresult setParamNormalized(Steinberg::uint32 id, double value);

    /**
     * @brief Register the bridge IComponentHandler with the controller.
     * @param handler  Caller-owned IComponentHandler (must outlive this object).
     * @return kResultOk on success.
     */
    Steinberg::tresult setComponentHandler(Steinberg::IComponentHandler* handler);

    // ---- IPlugView ----------------------------------------------------------

    /**
     * @brief Ask the controller to create the editor view.
     * @return true if the view was created successfully.
     */
    bool createView();

    /**
     * @brief Attach the plugin editor view to an HWND.
     * @param hwnd  Off-screen window handle.
     * @return kResultOk on success.
     */
    Steinberg::tresult attachView(HWND hwnd);

    /** Remove the view from its parent window. */
    Steinberg::tresult removeView();

    /**
     * @brief Get the preferred editor size.
     * @param rect  Output view rect.
     * @return kResultOk on success.
     */
    Steinberg::tresult getViewSize(Steinberg::ViewRect& rect);

    // ---- Interface accessors (for AudioProcessor) ---------------------------

    /** @return Raw IAudioProcessor pointer (not addRef'd). */
    Steinberg::IAudioProcessor* audioProcessor() const noexcept { return audioProc_; }

    /** @return Raw IEditController pointer (not addRef'd). */
    Steinberg::IEditController* editController() const noexcept { return controller_; }

    /** @return Raw IComponent pointer (not addRef'd). */
    Steinberg::IComponent* component() const noexcept { return component_; }

private:
    explicit PluginInstance(uint64_t id);

    uint64_t id_;

    Steinberg::IComponent*       component_   = nullptr;
    Steinberg::IAudioProcessor*  audioProc_   = nullptr;
    Steinberg::IEditController*  controller_  = nullptr;
    Steinberg::IPlugView*        view_        = nullptr;

    /** True when component and controller are the same object. */
    bool singleComponent_ = false;

    /** Atomically assigned instance IDs. */
    static uint64_t nextId_;
};

} // namespace vst3bridge
