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
 * @file host_main.cpp
 * @brief Wine host main loop.
 *
 * This is the Windows/Wine executable that:
 *  1. Connects back to the native Linux library via a Unix socket.
 *  2. Loads the Windows VST3 plugin DLL.
 *  3. Creates plugin instances and routes all VST3 interface calls from
 *     the native side to the plugin and back.
 *  4. Captures the plugin GUI via GDI and sends frames via shared memory.
 *  5. Injects X11 input events received from the native side as Windows
 *     messages into the plugin window.
 *
 * Threading model:
 *  - Main thread: Windows message pump + IPC message loop (interleaved).
 *  - GDI capture thread: captures plugin window frames at ~30 fps.
 *
 * All VST3 interface calls are serialised through the main thread.
 * Audio processing is synchronous (request → process → response) and
 * also runs on the main thread to avoid cross-thread window issues.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "logger.h"
#include "parameter_changes.h"
#include "vst3_host.h"
#include "plugin_instance.h"
#include "host_application.h"
#include "component_handler.h"
#include "window_manager.h"
#include "audio_processor.h"
#include "audio_shm_host.h"
#include "gdi_capture.h"
#include "gui_event_receiver.h"
#include "ipc_host.h"

#pragma comment(lib, "ws2_32.lib")

namespace vst3bridge {

// ============================================================================
// WineSocketClient — Winsock-based Unix socket client
// ============================================================================

/**
 * @brief Thin Winsock wrapper that speaks the same MessageHeader/payload
 *        framing protocol as the native side's POSIX MessageSocket.
 *
 * Wine maps AF_UNIX to native Unix domain sockets, so this works
 * transparently across the Wine / native boundary.
 */
#include "wine_socket_client.h"

// ============================================================================
// WineHost
// ============================================================================

/**
 * @brief Orchestrates the Wine host: plugin loading, message loop.
 */
class WineHost {
public:
    WineHost()  noexcept = default;
    ~WineHost()          { shutdown(); }

    WineHost(const WineHost&) = delete;
    WineHost& operator=(const WineHost&) = delete;

    // ---- Lifecycle ----------------------------------------------------------

    bool initialize(const std::string& socketPath,
                    const std::string& pluginPath)
    {
        // ---- 1. Connect to native side --------------------------------------
        if (!socket_.connect(socketPath)) {
            LOG_ERROR("WineHost: failed to connect to {}", socketPath);
            return false;
        }
        LOG_INFO("WineHost: connected to native side at {}", socketPath);

        // ---- 2. Load the plugin DLL -----------------------------------------
        // pluginPath is a UTF-8 string from the environment variable; convert.
        int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                       pluginPath.c_str(), -1,
                                       nullptr, 0);
        std::wstring wPath(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0,
                            pluginPath.c_str(), -1,
                            wPath.data(), wlen);

        if (!vst3Host_.loadPlugin(wPath.c_str())) {
            LOG_ERROR("WineHost: failed to load plugin: {}", pluginPath);
            return false;
        }

        // ---- 3. Initialise host application & component handler -------------
        hostApp_ = std::make_unique<HostApplication>();

        // ---- 4. Initialise window manager (for off-screen GUI capture) ------
        if (!windowManager_.initialize()) {
            LOG_WARN("WineHost: window manager init failed — no GUI support");
        }

        // ---- 5. Perform handshake -------------------------------------------
        if (!doHandshake()) {
            LOG_ERROR("WineHost: handshake failed");
            return false;
        }

        return true;
    }

    int run() {
        running_ = true;

        while (running_) {
            // Pump Windows messages (needed for plugin GUI)
            pumpMessages();

            // Try to receive one IPC message with a short timeout
            GenericMessage msg;
            if (socket_.receiveMessage(msg, /*timeoutMs=*/10)) {
                if (!handleMessage(msg)) {
                    LOG_WARN("WineHost: handleMessage returned false for type {}",
                             static_cast<uint32_t>(msg.header.type));
                }
            }
        }

        LOG_INFO("WineHost: main loop exited");
        return 0;
    }

    void shutdown() {
        if (captureThread_.joinable()) {
            stopCapture_ = true;
            captureThread_.join();
        }

        if (pluginInstance_) {
            pluginInstance_->terminate();
            pluginInstance_.reset();
        }

        componentHandler_.reset();
        hostApp_.reset();

        windowManager_.shutdown();
        vst3Host_.unloadPlugin();
        socket_.close();
    }

private:
    // ---- Handshake ----------------------------------------------------------

    bool doHandshake() {
        // The native side creates the socket server; Wine connects and sends
        // the handshake first.
        if (!socket_.sendMessage(MsgType::Handshake, nullptr, 0)) {
            return false;
        }

        GenericMessage resp;
        if (!socket_.receiveMessage(resp, /*timeoutMs=*/5000)) {
            LOG_ERROR("WineHost: timeout waiting for HandshakeResponse");
            return false;
        }

        if (resp.header.type != MsgType::HandshakeResponse) {
            LOG_ERROR("WineHost: expected HandshakeResponse, got type {}",
                      static_cast<uint32_t>(resp.header.type));
            return false;
        }

        if (resp.header.payload_size >= sizeof(MsgResponseHandshake)) {
            const auto* r = reinterpret_cast<const MsgResponseHandshake*>(
                resp.payload.data());
            if (r->protocol_version != PROTOCOL_VERSION) {
                LOG_ERROR("WineHost: protocol version mismatch: native={}, wine={}",
                          r->protocol_version, PROTOCOL_VERSION);
                return false;
            }
        }

        LOG_INFO("WineHost: handshake complete");
        return true;
    }

    // ---- Windows message pump -----------------------------------------------

    static void pumpMessages() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // ---- Message dispatcher -------------------------------------------------

    bool handleMessage(const GenericMessage& msg) {
        using T = MsgType;

        switch (msg.header.type) {

        // ---- Connection --------------------------------------------------------
        case T::Ping:
            return socket_.sendMessage(T::Pong, nullptr, 0);

        case T::Shutdown:
            running_ = false;
            return socket_.sendMessage(T::ShutdownAck, nullptr, 0);

        // ---- IPluginFactory ----------------------------------------------------
        case T::RequestFactoryInfo:
            return handleRequestFactoryInfo();

        case T::RequestClassCount:
            return handleRequestClassCount();

        case T::RequestClassInfo: {
            if (!checkPayload(msg, sizeof(MsgRequestClassInfo))) return false;
            return handleRequestClassInfo(
                *reinterpret_cast<const MsgRequestClassInfo*>(msg.payload.data()));
        }

        case T::RequestClassInfo2: {
            if (!checkPayload(msg, sizeof(MsgRequestClassInfo2))) return false;
            return handleRequestClassInfo2(
                *reinterpret_cast<const MsgRequestClassInfo2*>(msg.payload.data()));
        }

        case T::RequestClassInfoW: {
            if (!checkPayload(msg, sizeof(MsgRequestClassInfoW))) return false;
            return handleRequestClassInfoW(
                *reinterpret_cast<const MsgRequestClassInfoW*>(msg.payload.data()));
        }

        case T::CreateInstance: {
            if (!checkPayload(msg, sizeof(MsgRequestCreateInstance))) return false;
            return handleCreateInstance(
                *reinterpret_cast<const MsgRequestCreateInstance*>(msg.payload.data()));
        }

        // ---- IPluginBase -------------------------------------------------------
        case T::Initialize:
            return handleInitialize();

        case T::Terminate:
            return handleTerminate();

        // ---- IComponent --------------------------------------------------------
        case T::GetControllerClassId:
            return handleGetControllerClassId();

        case T::SetIoMode: {
            if (!checkPayload(msg, sizeof(MsgRequestSetIoMode))) return false;
            return handleSetIoMode(
                *reinterpret_cast<const MsgRequestSetIoMode*>(msg.payload.data()));
        }

        case T::GetBusCount: {
            if (!checkPayload(msg, sizeof(MsgRequestGetBusCount))) return false;
            return handleGetBusCount(
                *reinterpret_cast<const MsgRequestGetBusCount*>(msg.payload.data()));
        }

        case T::GetBusInfo: {
            if (!checkPayload(msg, sizeof(MsgRequestGetBusInfo))) return false;
            return handleGetBusInfo(
                *reinterpret_cast<const MsgRequestGetBusInfo*>(msg.payload.data()));
        }

        case T::ActivateBus: {
            if (!checkPayload(msg, sizeof(MsgRequestActivateBus))) return false;
            return handleActivateBus(
                *reinterpret_cast<const MsgRequestActivateBus*>(msg.payload.data()));
        }

        case T::SetActive: {
            if (!checkPayload(msg, sizeof(MsgRequestSetActive))) return false;
            return handleSetActive(
                *reinterpret_cast<const MsgRequestSetActive*>(msg.payload.data()));
        }

        case T::SetState:
            return handleSetState(msg);

        case T::GetState:
            return handleGetState();

        // ---- IAudioProcessor ---------------------------------------------------
        case T::SetBusArrangements: {
            if (!checkPayload(msg, sizeof(MsgRequestSetBusArrangements))) return false;
            return handleSetBusArrangements(
                *reinterpret_cast<const MsgRequestSetBusArrangements*>(msg.payload.data()));
        }

        case T::GetBusArrangement: {
            if (!checkPayload(msg, sizeof(MsgRequestGetBusArrangement))) return false;
            return handleGetBusArrangement(
                *reinterpret_cast<const MsgRequestGetBusArrangement*>(msg.payload.data()));
        }

        case T::CanProcessSampleSize: {
            if (!checkPayload(msg, sizeof(MsgRequestCanProcessSampleSize))) return false;
            return handleCanProcessSampleSize(
                *reinterpret_cast<const MsgRequestCanProcessSampleSize*>(msg.payload.data()));
        }

        case T::GetLatencySamples:
            return handleGetLatencySamples();

        case T::SetupProcessing: {
            if (!checkPayload(msg, sizeof(MsgRequestSetupProcessing))) return false;
            return handleSetupProcessing(
                *reinterpret_cast<const MsgRequestSetupProcessing*>(msg.payload.data()));
        }

        case T::SetProcessing: {
            if (!checkPayload(msg, sizeof(MsgRequestSetProcessing))) return false;
            return handleSetProcessing(
                *reinterpret_cast<const MsgRequestSetProcessing*>(msg.payload.data()));
        }

        case T::ParamChangesInput: {
            if (!checkPayload(msg, sizeof(MsgParamChanges))) return false;
            return handleParamChangesInput(msg);
        }

        case T::Process: {
            if (!checkPayload(msg, sizeof(MsgProcess))) return false;
            return handleProcess(
                *reinterpret_cast<const MsgProcess*>(msg.payload.data()));
        }

        case T::GetTailSamples:
            return handleGetTailSamples();

        // ---- IEditController ---------------------------------------------------
        case T::SetComponentState:
            return handleSetComponentState(msg);

        case T::GetParameterCount:
            return handleGetParameterCount();

        case T::GetParameterInfo: {
            if (!checkPayload(msg, sizeof(MsgRequestGetParameterInfo))) return false;
            return handleGetParameterInfo(
                *reinterpret_cast<const MsgRequestGetParameterInfo*>(msg.payload.data()));
        }

        case T::GetParamNormalized: {
            if (!checkPayload(msg, sizeof(MsgRequestGetParamNormalized))) return false;
            return handleGetParamNormalized(
                *reinterpret_cast<const MsgRequestGetParamNormalized*>(msg.payload.data()));
        }

        case T::SetParamNormalized: {
            if (!checkPayload(msg, sizeof(MsgRequestSetParamNormalized))) return false;
            return handleSetParamNormalized(
                *reinterpret_cast<const MsgRequestSetParamNormalized*>(msg.payload.data()));
        }

        case T::SetComponentHandler:
            return handleSetComponentHandler();

        case T::CreateView: {
            if (!checkPayload(msg, sizeof(MsgRequestCreateView))) return false;
            return handleCreateView(
                *reinterpret_cast<const MsgRequestCreateView*>(msg.payload.data()));
        }

        // ---- IPlugView ---------------------------------------------------------
        case T::ViewAttached: {
            if (!checkPayload(msg, sizeof(MsgRequestViewAttached))) return false;
            return handleViewAttached(
                *reinterpret_cast<const MsgRequestViewAttached*>(msg.payload.data()));
        }

        case T::ViewRemoved:
            return handleViewRemoved();

        case T::ViewGetSize:
            return handleViewGetSize();

        case T::ViewOnSize: {
            if (!checkPayload(msg, sizeof(MsgRequestViewOnSize))) return false;
            return handleViewOnSize(
                *reinterpret_cast<const MsgRequestViewOnSize*>(msg.payload.data()));
        }

        case T::ViewCanResize:
            return handleViewCanResize();

        // ---- Input events (native → wine) --------------------------------------
        case T::InputEvent: {
            if (!checkPayload(msg, sizeof(MsgInputEvent))) return false;
            return handleInputEvent(
                *reinterpret_cast<const MsgInputEvent*>(msg.payload.data()));
        }

        default:
            LOG_WARN("WineHost: unknown message type 0x{:x}",
                     static_cast<uint32_t>(msg.header.type));
            return false;
        }
    }

    // ---- Helper: validate payload size --------------------------------------

    static bool checkPayload(const GenericMessage& msg, size_t required) {
        if (msg.payload.size() < required) {
            LOG_ERROR("WineHost: payload too small for type 0x{:x}: got {} need {}",
                      static_cast<uint32_t>(msg.header.type),
                      msg.payload.size(), required);
            return false;
        }
        return true;
    }

    // =========================================================================
    // IPluginFactory handlers
    // =========================================================================

    bool handleRequestFactoryInfo() {
        auto* factory = vst3Host_.getFactory();
        MsgResponseFactoryInfo resp{};
        if (factory) {
            factory->getFactoryInfo(&resp.info);
        }
        return socket_.sendMessage(MsgType::ResponseFactoryInfo, &resp, sizeof(resp));
    }

    bool handleRequestClassCount() {
        auto* factory = vst3Host_.getFactory();
        MsgResponseClassCount resp{};
        resp.count = factory ? factory->countClasses() : 0;
        return socket_.sendMessage(MsgType::ResponseClassCount, &resp, sizeof(resp));
    }

    bool handleRequestClassInfo(const MsgRequestClassInfo& req) {
        auto* factory = vst3Host_.getFactory();
        MsgResponseClassInfo resp{};
        if (factory) {
            resp.success = (factory->getClassInfo(req.index, &resp.info) ==
                            Steinberg::kResultOk);
        }
        return socket_.sendMessage(MsgType::ResponseClassInfo, &resp, sizeof(resp));
    }

    bool handleRequestClassInfo2(const MsgRequestClassInfo2& req) {
        auto* f2 = vst3Host_.getFactory2();
        MsgResponseClassInfo2 resp{};
        if (f2) {
            resp.success = (f2->getClassInfo2(req.index, &resp.info) ==
                            Steinberg::kResultOk);
        }
        return socket_.sendMessage(MsgType::ResponseClassInfo2, &resp, sizeof(resp));
    }

    bool handleRequestClassInfoW(const MsgRequestClassInfoW& req) {
        auto* f3 = vst3Host_.getFactory3();
        MsgResponseClassInfoW resp{};
        if (f3) {
            resp.success = (f3->getClassInfoUnicode(req.index, &resp.info) ==
                            Steinberg::kResultOk);
        }
        return socket_.sendMessage(MsgType::ResponseClassInfoW, &resp, sizeof(resp));
    }

    bool handleCreateInstance(const MsgRequestCreateInstance& req) {
        MsgResponseCreateInstance resp{};

        auto* factory = vst3Host_.getFactory();
        if (!factory) {
            resp.success = false;
        } else {
            pluginInstance_ = PluginInstance::create(
                factory, req.cid, hostApp_.get());
            resp.success     = (pluginInstance_ != nullptr);
            resp.instance_id = resp.success ? pluginInstance_->getId() : 0;
        }

        LOG_INFO("WineHost: CreateInstance success={} id={}",
                 resp.success, resp.instance_id);
        return socket_.sendMessage(MsgType::ResponseCreateInstance, &resp, sizeof(resp));
    }

    // =========================================================================
    // IPluginBase handlers
    // =========================================================================

    bool handleInitialize() {
        Steinberg::tresult result = Steinberg::kInternalError;
        if (pluginInstance_) {
            result = pluginInstance_->initialize(hostApp_.get());
        }
        MsgResponseInitialize resp{};
        resp.result = static_cast<int32_t>(result);
        return socket_.sendMessage(MsgType::ResponseInitialize, &resp, sizeof(resp));
    }

    bool handleTerminate() {
        Steinberg::tresult result = Steinberg::kResultOk;
        if (pluginInstance_) {
            result = pluginInstance_->terminate();
        }
        MsgResponseTerminate resp{};
        resp.result = static_cast<int32_t>(result);
        return socket_.sendMessage(MsgType::ResponseTerminate, &resp, sizeof(resp));
    }

    // =========================================================================
    // IComponent handlers
    // =========================================================================

    bool handleGetControllerClassId() {
        MsgResponseGetControllerClassId resp{};
        if (pluginInstance_) {
            Steinberg::FUID classId;
            Steinberg::tresult res =
                pluginInstance_->getControllerClassId(classId);
            resp.success = (res == Steinberg::kResultOk);
            if (resp.success) {
                classId.toTUID(resp.classId);
            }
        }
        return socket_.sendMessage(MsgType::ResponseGetControllerClassId,
                                   &resp, sizeof(resp));
    }

    bool handleSetIoMode(const MsgRequestSetIoMode& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->setIoMode(
                static_cast<Steinberg::IoMode>(req.mode));
        }
        MsgResponseSetIoMode resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetIoMode, &resp, sizeof(resp));
    }

    bool handleGetBusCount(const MsgRequestGetBusCount& req) {
        MsgResponseGetBusCount resp{};
        if (pluginInstance_) {
            resp.count = pluginInstance_->getBusCount(
                static_cast<Steinberg::MediaType>(req.media_type),
                static_cast<Steinberg::BusDirection>(req.direction));
        }
        return socket_.sendMessage(MsgType::ResponseGetBusCount, &resp, sizeof(resp));
    }

    bool handleGetBusInfo(const MsgRequestGetBusInfo& req) {
        MsgResponseGetBusInfo resp{};
        if (pluginInstance_) {
            resp.result = static_cast<int32_t>(
                pluginInstance_->getBusInfo(
                    static_cast<Steinberg::MediaType>(req.media_type),
                    static_cast<Steinberg::BusDirection>(req.direction),
                    req.index,
                    resp.info));
        }
        return socket_.sendMessage(MsgType::ResponseGetBusInfo, &resp, sizeof(resp));
    }

    bool handleActivateBus(const MsgRequestActivateBus& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->activateBus(
                static_cast<Steinberg::MediaType>(req.media_type),
                static_cast<Steinberg::BusDirection>(req.direction),
                req.index, req.state);
        }
        MsgResponseActivateBus resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseActivateBus, &resp, sizeof(resp));
    }

    bool handleSetActive(const MsgRequestSetActive& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->setActive(req.state);
        }
        MsgResponseSetActive resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetActive, &resp, sizeof(resp));
    }

    bool handleSetState(const GenericMessage& msg) {
        // Header is MsgRequestSetState followed by data_size raw bytes on the socket.
        if (msg.payload.size() < sizeof(MsgRequestSetState)) {
            return false;
        }
        const auto* hdr = reinterpret_cast<const MsgRequestSetState*>(
            msg.payload.data());

        std::vector<uint8_t> data(hdr->data_size);
        if (hdr->data_size > 0) {
            if (!socket_.recvRaw(data.data(), hdr->data_size)) {
                LOG_ERROR("WineHost::handleSetState: failed to read state data");
                return false;
            }
        }

        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->setState(data);
        }

        MsgResponseSetState resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetState, &resp, sizeof(resp));
    }

    bool handleGetState() {
        std::vector<uint8_t> data;
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->getState(data);
        }

        MsgResponseGetState resp{};
        resp.result    = static_cast<int32_t>(res);
        resp.data_size = (res == Steinberg::kResultOk)
                        ? static_cast<uint32_t>(data.size())
                        : 0;

        if (!socket_.sendMessage(MsgType::ResponseGetState, &resp, sizeof(resp))) {
            return false;
        }

        // Followed by raw state bytes
        if (resp.data_size > 0) {
            if (!socket_.sendRaw(data.data(), data.size())) {
                return false;
            }
        }
        return true;
    }

    // =========================================================================
    // IAudioProcessor handlers
    // =========================================================================

    bool handleSetBusArrangements(const MsgRequestSetBusArrangements& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->setBusArrangements(req);
        }
        MsgResponseSetBusArrangements resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetBusArrangements,
                                   &resp, sizeof(resp));
    }

    bool handleGetBusArrangement(const MsgRequestGetBusArrangement& req) {
        MsgResponseGetBusArrangement resp{};
        if (pluginInstance_) {
            Steinberg::SpeakerArrangement arr = 0;
            resp.result = static_cast<int32_t>(
                pluginInstance_->getBusArrangement(
                    static_cast<Steinberg::BusDirection>(req.direction),
                    req.bus_index, arr));
            resp.arrangement = arr;
        }
        return socket_.sendMessage(MsgType::ResponseGetBusArrangement,
                                   &resp, sizeof(resp));
    }

    bool handleCanProcessSampleSize(const MsgRequestCanProcessSampleSize& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->canProcessSampleSize(req.symbolic_sample_size);
        }
        MsgResponseCanProcessSampleSize resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseCanProcessSampleSize,
                                   &resp, sizeof(resp));
    }

    bool handleGetLatencySamples() {
        MsgResponseGetLatencySamples resp{};
        if (pluginInstance_) {
            resp.latency = pluginInstance_->getLatencySamples();
        }
        return socket_.sendMessage(MsgType::ResponseGetLatencySamples,
                                   &resp, sizeof(resp));
    }

    bool handleSetupProcessing(const MsgRequestSetupProcessing& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            // Need a non-const copy because setupProcessing takes a reference
            Steinberg::ProcessSetup setup = req.setup;
            res = pluginInstance_->setupProcessing(setup);

            if (res == Steinberg::kResultOk) {
                // Create the audio processor
                if (!audioProcessor_) {
                    audioProcessor_ = std::make_unique<AudioProcessor>(
                        audioShm_.get(), &socket_,
                        pluginInstance_->audioProcessor(),
                        pluginInstance_->editController());
                }

                // After successful setup, send AudioReady so native side knows
                // the Wine host is prepared for audio processing.
                MsgAudioReady ready{};
                std::strncpy(ready.shm_name,
                             audioShmName_.c_str(),
                             sizeof(ready.shm_name) - 1);

                if (pluginInstance_->component()) {
                    // Fill bus channel counts
                    ready.input_bus_count  = static_cast<uint32_t>(
                        pluginInstance_->getBusCount(
                            Steinberg::kAudio, Steinberg::kInput));
                    ready.output_bus_count = static_cast<uint32_t>(
                        pluginInstance_->getBusCount(
                            Steinberg::kAudio, Steinberg::kOutput));

                    for (uint32_t i = 0; i < ready.input_bus_count && i < 8; ++i) {
                        Steinberg::SpeakerArrangement arr = 0;
                        pluginInstance_->getBusArrangement(
                            Steinberg::kInput, i, arr);
                        ready.input_bus_channels[i] = static_cast<uint32_t>(
                            Steinberg::SpeakerArr::getChannelCount(arr));
                    }
                    for (uint32_t i = 0; i < ready.output_bus_count && i < 8; ++i) {
                        Steinberg::SpeakerArrangement arr = 0;
                        pluginInstance_->getBusArrangement(
                            Steinberg::kOutput, i, arr);
                        ready.output_bus_channels[i] = static_cast<uint32_t>(
                            Steinberg::SpeakerArr::getChannelCount(arr));
                    }
                }

                socket_.sendMessage(MsgType::AudioReady, &ready, sizeof(ready));
            }
        }
        MsgResponseSetupProcessing resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetupProcessing,
                                   &resp, sizeof(resp));
    }

    bool handleSetProcessing(const MsgRequestSetProcessing& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->setProcessing(req.state);
        }
        MsgResponseSetProcessing resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetProcessing,
                                   &resp, sizeof(resp));
    }

    bool handleParamChangesInput(const GenericMessage& msg) {
        const MsgParamChanges* header = reinterpret_cast<const MsgParamChanges*>(msg.payload.data());
        const uint32_t numChanges = header->num_changes;
        const ParamChangePoint* changes = reinterpret_cast<const ParamChangePoint*>(
            msg.payload.data() + sizeof(MsgParamChanges));

        currentParamChanges_.clear();
        for (uint32_t i = 0; i < numChanges; ++i) {
            const ParamChangePoint& pc = changes[i];
            int32_t index = 0;
            Steinberg::IParamValueQueue* queue = currentParamChanges_.addParameterData(pc.param_id, index);
            if (queue) {
                queue->addPoint(pc.sample_offset, pc.value, index);
            }
        }
        return true;  // No response needed
    }

    /**
     * @brief Process one audio block.
     *
     * Audio data is in shared memory; the native side has already written
     * input samples there before sending this message.
     */
    bool handleProcess(const MsgProcess& req) {
        if (!pluginInstance_ || !audioShm_) {
            MsgResponseProcess resp{static_cast<int32_t>(Steinberg::kInternalError)};
            return socket_.sendMessage(MsgType::ResponseProcess, &resp, sizeof(resp));
        }

        const uint32_t numSamples = req.num_samples;

        if (numSamples == 0 || numSamples > AudioBufferLayout::kMaxSamples) {
            LOG_ERROR("WineHost::handleProcess: invalid num_samples {}",
                      numSamples);
            MsgResponseProcess resp{static_cast<int32_t>(Steinberg::kInternalError)};
            return socket_.sendMessage(MsgType::ResponseProcess, &resp, sizeof(resp));
        }

        // --- Build ProcessData ---
        const uint32_t nIn  = audioShm_->getNumInputs();
        const uint32_t nOut = audioShm_->getNumOutputs();

        inputBuses_.resize(nIn);
        outputBuses_.resize(nOut);
        inputPtrs_.resize(nIn);
        outputPtrs_.resize(nOut);

        for (uint32_t b = 0; b < nIn; ++b) {
            const uint32_t nCh = 1;  // Single channel per bus for simplicity
            inputBuses_[b].numChannels  = static_cast<Steinberg::int32>(nCh);
            inputBuses_[b].silenceFlags = 0;
            inputPtrs_[b].resize(nCh);
            for (uint32_t ch = 0; ch < nCh; ++ch) {
                inputPtrs_[b][ch] = audioShm_->getInputBuffer(b);
            }
            inputBuses_[b].channelBuffers32 = inputPtrs_[b].data();
        }

        for (uint32_t b = 0; b < nOut; ++b) {
            const uint32_t nCh = 1;  // Single channel per bus for simplicity
            outputBuses_[b].numChannels  = static_cast<Steinberg::int32>(nCh);
            outputBuses_[b].silenceFlags = 0;
            outputPtrs_[b].resize(nCh);
            for (uint32_t ch = 0; ch < nCh; ++ch) {
                outputPtrs_[b][ch] = audioShm_->getOutputBuffer(b);
            }
            outputBuses_[b].channelBuffers32 = outputPtrs_[b].data();
        }

        Steinberg::ProcessData data{};
        data.processMode            = Steinberg::kRealtime;
        data.symbolicSampleSize     = Steinberg::kSample32;
        data.numSamples             = static_cast<Steinberg::int32>(numSamples);
        data.numInputs              = static_cast<Steinberg::int32>(nIn);
        data.numOutputs             = static_cast<Steinberg::int32>(nOut);
        data.inputs                 = nIn  ? inputBuses_.data()  : nullptr;
        data.outputs                = nOut ? outputBuses_.data() : nullptr;
        data.inputParameterChanges  = &currentParamChanges_;
        // this message arrives (if any were queued).
        data.inputParameterChanges  = nullptr;
        data.outputParameterChanges = nullptr;
        data.inputEvents            = nullptr;
        data.outputEvents           = nullptr;
        data.processContext         = nullptr;

        Steinberg::tresult res = pluginInstance_->process(data);

        MsgResponseProcess resp{static_cast<int32_t>(res)};
        return socket_.sendMessage(MsgType::ResponseProcess, &resp, sizeof(resp));
    }

    bool handleGetTailSamples() {
        MsgResponseGetTailSamples resp{};
        if (pluginInstance_) {
            resp.tail = pluginInstance_->getTailSamples();
        }
        return socket_.sendMessage(MsgType::ResponseGetTailSamples,
                                   &resp, sizeof(resp));
    }

    // =========================================================================
    // IEditController handlers
    // =========================================================================

    bool handleSetComponentState(const GenericMessage& msg) {
        if (msg.payload.size() < sizeof(MsgRequestSetComponentState)) return false;
        const auto* hdr = reinterpret_cast<const MsgRequestSetComponentState*>(
            msg.payload.data());

        std::vector<uint8_t> data(hdr->data_size);
        if (hdr->data_size > 0) {
            if (!socket_.recvRaw(data.data(), hdr->data_size)) return false;
        }

        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->setControllerComponentState(data);
        }

        MsgResponseSetComponentState resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetComponentState,
                                   &resp, sizeof(resp));
    }

    bool handleGetParameterCount() {
        MsgResponseGetParameterCount resp{};
        if (pluginInstance_) {
            resp.count = pluginInstance_->getParameterCount();
        }
        return socket_.sendMessage(MsgType::ResponseGetParameterCount,
                                   &resp, sizeof(resp));
    }

    bool handleGetParameterInfo(const MsgRequestGetParameterInfo& req) {
        MsgResponseGetParameterInfo resp{};
        if (pluginInstance_) {
            resp.result = static_cast<int32_t>(
                pluginInstance_->getParameterInfo(req.index, resp.info));
        }
        return socket_.sendMessage(MsgType::ResponseGetParameterInfo,
                                   &resp, sizeof(resp));
    }

    bool handleGetParamNormalized(const MsgRequestGetParamNormalized& req) {
        MsgResponseGetParamNormalized resp{};
        if (pluginInstance_) {
            resp.value = pluginInstance_->getParamNormalized(req.id);
        }
        return socket_.sendMessage(MsgType::ResponseGetParamNormalized,
                                   &resp, sizeof(resp));
    }

    bool handleSetParamNormalized(const MsgRequestSetParamNormalized& req) {
        Steinberg::tresult res = Steinberg::kInternalError;
        if (pluginInstance_) {
            res = pluginInstance_->setParamNormalized(req.id, req.value);
        }
        MsgResponseSetParamNormalized resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetParamNormalized,
                                   &resp, sizeof(resp));
    }

    bool handleSetComponentHandler() {
        Steinberg::tresult res = Steinberg::kNotImplemented;
        if (pluginInstance_) {
            if (!componentHandler_) {
                // Create the Wine-side IComponentHandler proxy.
                componentHandler_ = std::make_unique<ComponentHandler>(
                    &socket_);
            }
            res = pluginInstance_->setComponentHandler(
                componentHandler_.get());
        }
        MsgResponseSetComponentHandler resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseSetComponentHandler,
                                   &resp, sizeof(resp));
    }

    bool handleCreateView(const MsgRequestCreateView& /*req*/) {
        MsgResponseCreateView resp{};
        resp.success = false;
        resp.width   = 0;
        resp.height  = 0;
        std::memset(resp.frame_shm_name, 0, sizeof(resp.frame_shm_name));

        if (pluginInstance_ && pluginInstance_->createView()) {
            // Create an off-screen window for the plugin to render into
            pluginHwnd_ = windowManager_.createWindow(800, 600);  // initial size

            if (pluginHwnd_) {
                Steinberg::tresult attachRes =
                    pluginInstance_->attachView(pluginHwnd_);

                if (attachRes == Steinberg::kResultOk) {
                    // Query actual size from plugin
                    Steinberg::ViewRect rect{};
                    if (pluginInstance_->getViewSize(rect) == Steinberg::kResultOk) {
                        resp.width  = rect.getWidth();
                        resp.height = rect.getHeight();
                        windowManager_.resizeWindow(
                            pluginHwnd_, resp.width, resp.height);
                    }

                    // Initialise GDI capture
                    gdiCapture_ = std::make_unique<GDICapture>();
                    if (gdiCapture_->initialize(pluginHwnd_)) {
                        // Frame SHM name is communicated via MsgAudioReady
                        // (reused field); for clean design it's in this response.
                        std::strncpy(resp.frame_shm_name,
                                     frameShmName_.c_str(),
                                     sizeof(resp.frame_shm_name) - 1);
                        resp.success = true;

                        // Start capture thread
                        stopCapture_ = false;
                        captureThread_ = std::thread(
                            &WineHost::captureLoop, this);

                        // Start GUI event receiver
                        guiEventReceiver_ = std::make_unique<GUIEventReceiver>(
                            &socket_, pluginHwnd_);
                        guiEventReceiver_->start();
                    }
                }
            }
        }

        return socket_.sendMessage(MsgType::ResponseCreateView, &resp, sizeof(resp));
    }

    // =========================================================================
    // IPlugView handlers
    // =========================================================================

    bool handleViewAttached(const MsgRequestViewAttached& /*req*/) {
        // On the Wine side the view is already attached in handleCreateView().
        // This message is a no-op; just acknowledge.
        MsgResponseViewAttached resp{};
        resp.result = static_cast<int32_t>(Steinberg::kResultOk);
        return socket_.sendMessage(MsgType::ResponseViewAttached, &resp, sizeof(resp));
    }

    bool handleViewRemoved() {
        Steinberg::tresult res = Steinberg::kResultOk;
        if (pluginInstance_) {
            res = pluginInstance_->removeView();
        }
        if (pluginHwnd_) {
            windowManager_.destroyWindow(pluginHwnd_);
            pluginHwnd_ = nullptr;
        }
        MsgResponseViewRemoved resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseViewRemoved, &resp, sizeof(resp));
    }

    bool handleViewGetSize() {
        MsgResponseViewGetSize resp{};
        if (pluginInstance_) {
            resp.result = static_cast<int32_t>(
                pluginInstance_->getViewSize(resp.rect));
        }
        return socket_.sendMessage(MsgType::ResponseViewGetSize, &resp, sizeof(resp));
    }

    bool handleViewOnSize(const MsgRequestViewOnSize& req) {
        Steinberg::tresult res = Steinberg::kResultOk;
        if (pluginHwnd_) {
            windowManager_.resizeWindow(
                pluginHwnd_,
                req.rect.getWidth(),
                req.rect.getHeight());
        }
        MsgResponseViewOnSize resp{};
        resp.result = static_cast<int32_t>(res);
        return socket_.sendMessage(MsgType::ResponseViewOnSize, &resp, sizeof(resp));
    }

    bool handleViewCanResize() {
        MsgResponseViewCanResize resp{};
        resp.result = static_cast<int32_t>(Steinberg::kResultFalse);
        // TODO: query IPlugView::canResize()
        return socket_.sendMessage(MsgType::ResponseViewCanResize, &resp, sizeof(resp));
    }

    // =========================================================================
    // Input event injection
    // =========================================================================

    bool handleInputEvent(const MsgInputEvent& ev) {
        if (!pluginHwnd_) {
            MsgInputAck ack{false};
            return socket_.sendMessage(MsgType::InputAck, &ack, sizeof(ack));
        }

        bool handled = true;

        switch (ev.type) {
        case MsgInputEvent::Type::MouseMove:
            PostMessageW(pluginHwnd_, WM_MOUSEMOVE,
                         0, MAKELPARAM(ev.x, ev.y));
            break;
        case MsgInputEvent::Type::MouseDown:
            if (ev.button == 1)
                PostMessageW(pluginHwnd_, WM_LBUTTONDOWN, MK_LBUTTON,
                             MAKELPARAM(ev.x, ev.y));
            else if (ev.button == 2)
                PostMessageW(pluginHwnd_, WM_MBUTTONDOWN, MK_MBUTTON,
                             MAKELPARAM(ev.x, ev.y));
            else if (ev.button == 3)
                PostMessageW(pluginHwnd_, WM_RBUTTONDOWN, MK_RBUTTON,
                             MAKELPARAM(ev.x, ev.y));
            break;
        case MsgInputEvent::Type::MouseUp:
            if (ev.button == 1)
                PostMessageW(pluginHwnd_, WM_LBUTTONUP, 0,
                             MAKELPARAM(ev.x, ev.y));
            else if (ev.button == 2)
                PostMessageW(pluginHwnd_, WM_MBUTTONUP, 0,
                             MAKELPARAM(ev.x, ev.y));
            else if (ev.button == 3)
                PostMessageW(pluginHwnd_, WM_RBUTTONUP, 0,
                             MAKELPARAM(ev.x, ev.y));
            break;
        case MsgInputEvent::Type::MouseWheel: {
            const WORD delta = static_cast<WORD>(
                static_cast<int>(ev.delta * WHEEL_DELTA));
            PostMessageW(pluginHwnd_, WM_MOUSEWHEEL,
                         MAKEWPARAM(0, delta),
                         MAKELPARAM(ev.x, ev.y));
            break;
        }
        case MsgInputEvent::Type::KeyDown:
            PostMessageW(pluginHwnd_, WM_KEYDOWN,
                         static_cast<WPARAM>(ev.x), 0);
            break;
        case MsgInputEvent::Type::KeyUp:
            PostMessageW(pluginHwnd_, WM_KEYUP,
                         static_cast<WPARAM>(ev.x), 0);
            break;
        case MsgInputEvent::Type::FocusIn:
            PostMessageW(pluginHwnd_, WM_SETFOCUS, 0, 0);
            break;
        case MsgInputEvent::Type::FocusOut:
            PostMessageW(pluginHwnd_, WM_KILLFOCUS, 0, 0);
            break;
        default:
            handled = false;
            break;
        }

        MsgInputAck ack{handled};
        return socket_.sendMessage(MsgType::InputAck, &ack, sizeof(ack));
    }

    // =========================================================================
    // GDI capture loop (runs in separate thread)
    // =========================================================================

    void captureLoop() {
        constexpr int kTargetFps  = 30;
        constexpr int kFrameMs    = 1000 / kTargetFps;

        while (!stopCapture_.load(std::memory_order_relaxed)) {
            if (!gdiCapture_ || !frameShm_) {
                Sleep(kFrameMs);
                continue;
            }

            const int w = gdiCapture_->getWidth();
            const int h = gdiCapture_->getHeight();
            if (w <= 0 || h <= 0) {
                Sleep(kFrameMs);
                continue;
            }

            uint8_t* slot = frameShm_->beginWrite(
                static_cast<uint32_t>(w),
                static_cast<uint32_t>(h));
            if (!slot) {
                Sleep(kFrameMs);
                continue;
            }

            // Capture directly into the SHM slot
            bool ok = gdiCapture_->capture(slot, w, h);
            frameShm_->endWrite();

            if (ok) {
                MsgFrameUpdate fu{};
                fu.frame_id    = ++frameCounter_;
                fu.width       = static_cast<uint32_t>(w);
                fu.height      = static_cast<uint32_t>(h);
                fu.format      = 0;  // BGRA32
                fu.dirty_x     = 0;
                fu.dirty_y     = 0;
                fu.dirty_width  = fu.width;
                fu.dirty_height = fu.height;

                // Fire-and-forget; no wait for FrameAck in the capture thread
                // (the main loop handles FrameAck messages)
                socket_.sendMessage(MsgType::FrameUpdate, &fu, sizeof(fu));
            }

            Sleep(kFrameMs);
        }
    }

    // =========================================================================
    // Data members
    // =========================================================================

    WineSocketClient                  socket_;
    VST3Host                          vst3Host_;
    vst3bridge::ParameterChanges      currentParamChanges_;
    WindowManager                     windowManager_;
    std::unique_ptr<HostApplication>  hostApp_;
    std::unique_ptr<PluginInstance>   pluginInstance_;
    std::unique_ptr<AudioProcessor>   audioProcessor_;
    std::unique_ptr<ComponentHandler> componentHandler_;
    std::unique_ptr<GUIEventReceiver> guiEventReceiver_;
    std::unique_ptr<GDICapture>       gdiCapture_;
    std::unique_ptr<AudioSharedMemoryHost> audioShm_;
    // Frame shared memory is managed separately (owned by native side)
    // Wine side opens it by name received in environment
    struct FrameShmStub {
        uint8_t* beginWrite(uint32_t, uint32_t) { return nullptr; }
        void     endWrite() {}
    };
    // TODO: replace with real FrameSharedMemory once SHM is integrated
    std::unique_ptr<FrameShmStub>     frameShm_;

    HWND                              pluginHwnd_   = nullptr;
    std::string                       audioShmName_;
    std::string                       frameShmName_;

    // Audio processing scratch buffers (reused across calls)
    std::vector<Steinberg::AudioBusBuffers>   inputBuses_;
    std::vector<Steinberg::AudioBusBuffers>   outputBuses_;
    std::vector<std::vector<float*>>          inputPtrs_;
    std::vector<std::vector<float*>>          outputPtrs_;

    std::thread       captureThread_;
    std::atomic<bool> stopCapture_{false};
    uint32_t          frameCounter_{0};

    bool              running_ = false;
};

// ============================================================================
// Environment helpers
// ============================================================================

static std::string getEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// ============================================================================
// Entry point
// ============================================================================

static int runHost() {
    const std::string socketPath = getEnv("VST3BRIDGE_SOCKET_PATH");
    const std::string pluginPath = getEnv("VST3BRIDGE_PLUGIN_PATH");

    if (socketPath.empty()) {
        LOG_ERROR("VST3BRIDGE_SOCKET_PATH is not set");
        return 1;
    }
    if (pluginPath.empty()) {
        LOG_ERROR("VST3BRIDGE_PLUGIN_PATH is not set");
        return 1;
    }

    WineHost host;
    if (!host.initialize(socketPath, pluginPath)) {
        return 1;
    }

    return host.run();
}

} // namespace vst3bridge

// ============================================================================
// Windows entry point
// ============================================================================

int WINAPI WinMain(HINSTANCE /*hInstance*/,
                   HINSTANCE /*hPrevInstance*/,
                   LPSTR     /*lpCmdLine*/,
                   int       /*nCmdShow*/)
{
    return vst3bridge::runHost();
}
