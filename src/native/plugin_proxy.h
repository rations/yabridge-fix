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
 * @file plugin_proxy.h
 * @brief Native-side VST3 plugin instance proxy.
 *
 * PluginProxy implements IComponent, IAudioProcessor, and IEditController
 * for the DAW.  All calls are serialised and forwarded to the Wine host
 * process over a Unix socket.  Audio data is exchanged via POSIX shared
 * memory to avoid copying through the socket.
 *
 * Thread-safety:
 *   The DAW may call this object from multiple threads:
 *    - The audio thread calls process() continuously.
 *    - The UI/main thread calls all other methods.
 *
 *   We serialise all socket access through a mutex (socket_mutex_).
 *   process() is special: it must be real-time safe.  It acquires the
 *   mutex briefly for the send/receive but does NOT block for long because
 *   the socket operations are non-blocking with a selectively short timeout.
 *   Audio data itself is never copied through the socket — it goes through
 *   shared memory.
 *
 *   IComponentHandler callbacks arrive from the main loop (ipc_listen thread)
 *   and are forwarded to the DAW-provided handler; that forwarding is likewise
 *   protected by a mutex.
 */

#pragma once

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"
#include "protocol.h"
#include "socket.h"
#include "shared_memory.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace vst3bridge {

// Forward declarations
class PlugViewProxy;

/**
 * @brief Native-side proxy implementing the full VST3 plugin interface.
 *
 * Created by PluginFactory when the DAW calls IPluginFactory::createInstance().
 * Destroyed when the DAW releases its last reference.
 */
class PluginProxy : public Steinberg::IComponent,
                     public Steinberg::IAudioProcessor,
                     public Steinberg::IEditController
{
public:
    /**
     * @param socket       Connected socket to the Wine host (shared ownership
     *                     so PluginFactory can demultiplex incoming messages).
     * @param instance_id  Instance ID assigned by the Wine host.
     */
    PluginProxy(std::shared_ptr<MessageSocket> socket, uint64_t instance_id);
    ~PluginProxy() override;

    // Non-copyable and non-movable (managed by raw pointers via COM ref-count)
    PluginProxy(const PluginProxy&) = delete;
    PluginProxy& operator=(const PluginProxy&) = delete;

    // ---- FUnknown -----------------------------------------------------------

    Steinberg::tresult PLUGIN_API queryInterface(
            const Steinberg::TUID _iid, void** obj) override;
    Steinberg::uint32  PLUGIN_API addRef()  override;
    Steinberg::uint32  PLUGIN_API release() override;

    // ---- IPluginBase (via IComponent) ---------------------------------------

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    // ---- IComponent ---------------------------------------------------------

    Steinberg::tresult PLUGIN_API getControllerClassId(
            Steinberg::FUID* classId) override;
    Steinberg::tresult PLUGIN_API setIoMode(Steinberg::IoMode mode) override;
    Steinberg::int32   PLUGIN_API getBusCount(
            Steinberg::MediaType type,
            Steinberg::BusDirection dir) override;
    Steinberg::tresult PLUGIN_API getBusInfo(
            Steinberg::MediaType type,
            Steinberg::BusDirection dir,
            Steinberg::int32  index,
            Steinberg::BusInfo& bus) override;
    Steinberg::tresult PLUGIN_API getRoutingInfo(
            Steinberg::RoutingInfo& inInfo,
            Steinberg::RoutingInfo& outInfo) override;
    Steinberg::tresult PLUGIN_API activateBus(
            Steinberg::MediaType type,
            Steinberg::BusDirection dir,
            Steinberg::int32 index,
            bool state) override;
    Steinberg::tresult PLUGIN_API setActive(bool state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

    // ---- IAudioProcessor ----------------------------------------------------

    Steinberg::tresult PLUGIN_API setBusArrangements(
            Steinberg::SpeakerArrangement* inputs,  Steinberg::int32 numIns,
            Steinberg::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API getBusArrangement(
            Steinberg::BusDirection dir,
            Steinberg::int32 busIndex,
            Steinberg::SpeakerArrangement& arr) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(
            Steinberg::int32 symbolicSampleSize) override;
    Steinberg::uint32  PLUGIN_API getLatencySamples() override;
    Steinberg::tresult PLUGIN_API setupProcessing(
            Steinberg::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API setProcessing(bool state) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::ProcessData& data) override;
    Steinberg::uint32  PLUGIN_API getTailSamples() override;

    // ---- IEditController ----------------------------------------------------

    Steinberg::tresult PLUGIN_API setComponentState(
            Steinberg::IBStream* state) override;
    Steinberg::int32   PLUGIN_API getParameterCount() override;
    Steinberg::tresult PLUGIN_API getParameterInfo(
            Steinberg::int32 paramIndex,
            Steinberg::ParameterInfo& info) override;
    Steinberg::tresult PLUGIN_API getParamStringByValue(
            Steinberg::uint32 id,
            double valueNormalized,
            Steinberg::char16* string) override;
    Steinberg::tresult PLUGIN_API getParamValueByString(
            Steinberg::uint32 id,
            Steinberg::char16* string,
            double& valueNormalized) override;
    double             PLUGIN_API getParamNormalized(Steinberg::uint32 id) override;
    Steinberg::tresult PLUGIN_API setParamNormalized(
            Steinberg::uint32 id, double value) override;
    Steinberg::tresult PLUGIN_API setComponentHandler(
            Steinberg::IComponentHandler* handler) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    // ---- Bridge-internal ----------------------------------------------------

    uint64_t getInstanceId() const noexcept { return instance_id_; }

    /**
     * @brief Called by the socket-listener thread when a ComponentHandler
     *        callback message arrives from the Wine host.
     *
     * Dispatches to the DAW-provided IComponentHandler stored by
     * setComponentHandler().
     */
    void handleComponentHandlerMessage(const GenericMessage& msg);

    /**
     * @brief Attach the audio shared memory after Wine host sends AudioReady.
     *
     * @param name  POSIX shared memory name received in MsgAudioReady.
     * @return true on success.
     */
    bool openAudioSharedMemory(const std::string& name);

private:
    // ---- Helpers ------------------------------------------------------------

    /**
     * @brief Send a request, wait for a specific response type, and decode it.
     *
     * Template method so each call site just passes the request and response struct.
     * NOT real-time safe (acquires socket_mutex_).
     */
    template<typename Req, typename Resp>
    bool sendRequest(MsgType reqType, const Req& req, Resp& resp) {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (!socket_->sendMessage(reqType, &req, sizeof(req))) {
            return false;
        }
        MsgType respType;
        return socket_->receiveMessage(&resp, sizeof(resp), respType);
    }

    /**
     * @brief Send a request with no payload, decode fixed-size response.
     */
    template<typename Resp>
    bool sendEmptyRequest(MsgType reqType, Resp& resp) {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (!socket_->sendMessage(reqType, nullptr, 0)) {
            return false;
        }
        MsgType respType;
        return socket_->receiveMessage(&resp, sizeof(resp), respType);
    }

    /**
     * @brief Read exactly @p size raw bytes from the socket (state data).
     *  Must be called while socket_mutex_ is held.
     */
    bool readRawBytes(std::vector<uint8_t>& buf, uint32_t size);

    /**
     * @brief Send raw bytes after a message header.
     *  Must be called while socket_mutex_ is held.
     */
    bool sendRawBytes(const void* data, size_t size);

    // ---- Members ------------------------------------------------------------

    std::shared_ptr<MessageSocket>   socket_;
    std::mutex                       socket_mutex_;

    uint64_t                         instance_id_;
    std::atomic<Steinberg::uint32>   ref_count_{1};

    /// DAW-provided IComponentHandler; called when Wine host reports GUI edits
    Steinberg::IComponentHandler*    component_handler_ = nullptr;
    std::mutex                       handler_mutex_;

    /// Audio shared memory (opened after AudioReady message)
    std::unique_ptr<AudioSharedMemory> audio_shm_;

    /// IPlugView proxy (created on demand by createView())
    std::shared_ptr<PlugViewProxy>   view_proxy_;
};

} // namespace vst3bridge
