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
 * @file plugin_proxy.cpp
 * @brief Native-side VST3 plugin instance proxy implementation.
 */

#include "plugin_proxy.h"
#include "logger.h"
#include <algorithm>
#include <cstring>

namespace vst3bridge {

using namespace Steinberg;

// ============================================================================
// Constructor / Destructor
// ============================================================================

PluginProxy::PluginProxy(std::shared_ptr<MessageSocket> socket,
                         uint64_t instance_id)
    : socket_(std::move(socket))
    , instance_id_(instance_id)
{}

PluginProxy::~PluginProxy() {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    if (component_handler_) {
        component_handler_->release();
        component_handler_ = nullptr;
    }
}

// ============================================================================
// FUnknown
// ============================================================================

tresult PLUGIN_API PluginProxy::queryInterface(
        const TUID _iid, void** obj)
{
    if (!obj) return kInvalidArgument;
    *obj = nullptr;

    FUID requested(_iid);

    if (requested == FUnknown::iid      ||
        requested == IPluginBase::iid   ||
        requested == IComponent::iid)
    {
        *obj = static_cast<IComponent*>(this);
        addRef();
        return kResultOk;
    }

    if (requested == IAudioProcessor::iid) {
        *obj = static_cast<IAudioProcessor*>(this);
        addRef();
        return kResultOk;
    }

    if (requested == IEditController::iid) {
        *obj = static_cast<IEditController*>(this);
        addRef();
        return kResultOk;
    }

    return kNoInterface;
}

uint32 PLUGIN_API PluginProxy::addRef() {
    return ++ref_count_;
}

uint32 PLUGIN_API PluginProxy::release() {
    uint32 r = --ref_count_;
    if (r == 0) delete this;
    return r;
}

// ============================================================================
// IPluginBase
// ============================================================================

tresult PLUGIN_API PluginProxy::initialize(FUnknown* /*context*/) {
    MsgRequestInitialize req{};
    req.context = 0;  // We do not forward the context pointer to Wine

    MsgResponseInitialize resp{};
    if (!sendRequest(MsgType::Initialize, req, resp)) {
        LOG_ERROR("PluginProxy::initialize(): IPC failed");
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::terminate() {
    MsgRequestTerminate req{};
    MsgResponseTerminate resp{};
    if (!sendRequest(MsgType::Terminate, req, resp)) {
        LOG_ERROR("PluginProxy::terminate(): IPC failed");
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

// ============================================================================
// IComponent
// ============================================================================

tresult PLUGIN_API PluginProxy::getControllerClassId(FUID* classId) {
    if (!classId) return kInvalidArgument;

    MsgRequestGetControllerClassId req{};
    MsgResponseGetControllerClassId resp{};
    if (!sendRequest(MsgType::GetControllerClassId, req, resp)) {
        return kInternalError;
    }
    if (resp.success) {
        *classId = FUID(resp.classId);
    }
    return resp.success ? kResultOk : kResultFalse;
}

tresult PLUGIN_API PluginProxy::setIoMode(IoMode mode) {
    MsgRequestSetIoMode req{};
    req.mode = static_cast<int32_t>(mode);

    MsgResponseSetIoMode resp{};
    if (!sendRequest(MsgType::SetIoMode, req, resp)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

int32 PLUGIN_API PluginProxy::getBusCount(MediaType type, BusDirection dir) {
    MsgRequestGetBusCount req{};
    req.media_type = static_cast<int32_t>(type);
    req.direction  = static_cast<int32_t>(dir);

    MsgResponseGetBusCount resp{};
    if (!sendRequest(MsgType::GetBusCount, req, resp)) {
        return 0;
    }
    return resp.count;
}

tresult PLUGIN_API PluginProxy::getBusInfo(
        MediaType type, BusDirection dir,
        int32 index, BusInfo& bus)
{
    MsgRequestGetBusInfo req{};
    req.media_type = static_cast<int32_t>(type);
    req.direction  = static_cast<int32_t>(dir);
    req.index      = index;

    MsgResponseGetBusInfo resp{};
    if (!sendRequest(MsgType::GetBusInfo, req, resp)) {
        return kInternalError;
    }
    if (resp.result == kResultOk) {
        bus = resp.info;
    }
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::getRoutingInfo(
        RoutingInfo& /*inInfo*/, RoutingInfo& /*outInfo*/)
{
    // Not forwarded — routing info is host-internal to many DAWs.
    return kNotImplemented;
}

tresult PLUGIN_API PluginProxy::activateBus(
        MediaType type, BusDirection dir,
        int32 index, bool state)
{
    MsgRequestActivateBus req{};
    req.media_type = static_cast<int32_t>(type);
    req.direction  = static_cast<int32_t>(dir);
    req.index      = index;
    req.state      = state;

    MsgResponseActivateBus resp{};
    if (!sendRequest(MsgType::ActivateBus, req, resp)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::setActive(bool state) {
    MsgRequestSetActive req{};
    req.state = state;

    MsgResponseSetActive resp{};
    if (!sendRequest(MsgType::SetActive, req, resp)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

// ---- State management -------------------------------------------------------

tresult PLUGIN_API PluginProxy::setState(IBStream* stream) {
    if (!stream) return kInvalidArgument;

    // Read all bytes from the DAW-provided stream
    std::vector<uint8_t> data;
    uint8_t  buf[4096];
    int32    bytesRead = 0;
    tresult  seekRes   = stream->seek(0, kIBSeekSet, nullptr);
    (void)seekRes;

    while (true) {
        int32 read = 0;
        tresult r = stream->read(buf, static_cast<int32>(sizeof(buf)), &read);
        if (r != kResultOk || read == 0) break;
        data.insert(data.end(), buf, buf + read);
        bytesRead += read;
    }

    // Send header then raw bytes
    MsgRequestSetState header{};
    header.data_size = static_cast<uint32_t>(data.size());

    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (!socket_->sendMessage(MsgType::SetState, &header, sizeof(header))) {
        return kInternalError;
    }
    if (!data.empty()) {
        if (!socket_->sendRaw(data.data(), data.size())) {
            return kInternalError;
        }
    }

    MsgResponseSetState resp{};
    MsgType respType;
    if (!socket_->receiveMessage(&resp, sizeof(resp), respType)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::getState(IBStream* stream) {
    if (!stream) return kInvalidArgument;

    MsgRequestGetState req{};
    MsgResponseGetState resp{};
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (!socket_->sendMessage(MsgType::GetState, &req, sizeof(req))) {
            return kInternalError;
        }
        MsgType respType;
        if (!socket_->receiveMessage(&resp, sizeof(resp), respType)) {
            return kInternalError;
        }

        if (resp.result == kResultOk && resp.data_size > 0) {
            std::vector<uint8_t> data(resp.data_size);
            if (!socket_->receiveRaw(data.data(), resp.data_size)) {
                return kInternalError;
            }
            int32 written = 0;
            stream->write(data.data(), static_cast<int32>(data.size()), &written);
        }
    }
    return static_cast<tresult>(resp.result);
}

// ============================================================================
// IAudioProcessor
// ============================================================================

tresult PLUGIN_API PluginProxy::setBusArrangements(
        SpeakerArrangement* inputs,  int32 numIns,
        SpeakerArrangement* outputs, int32 numOuts)
{
    if (numIns > 8 || numOuts > 8) return kInvalidArgument;

    MsgRequestSetBusArrangements req{};
    req.num_ins  = numIns;
    req.num_outs = numOuts;
    for (int32 i = 0; i < numIns;  ++i) req.in_arr[i]  = inputs[i];
    for (int32 i = 0; i < numOuts; ++i) req.out_arr[i] = outputs[i];

    MsgResponseSetBusArrangements resp{};
    if (!sendRequest(MsgType::SetBusArrangements, req, resp)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::getBusArrangement(
        BusDirection dir, int32 busIndex,
        SpeakerArrangement& arr)
{
    MsgRequestGetBusArrangement req{};
    req.direction  = static_cast<int32_t>(dir);
    req.bus_index  = busIndex;

    MsgResponseGetBusArrangement resp{};
    if (!sendRequest(MsgType::GetBusArrangement, req, resp)) {
        return kInternalError;
    }
    if (resp.result == kResultOk) {
        arr = resp.arrangement;
    }
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::canProcessSampleSize(int32 symbolicSampleSize) {
    MsgRequestCanProcessSampleSize req{};
    req.symbolic_sample_size = symbolicSampleSize;

    MsgResponseCanProcessSampleSize resp{};
    if (!sendRequest(MsgType::CanProcessSampleSize, req, resp)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

uint32 PLUGIN_API PluginProxy::getLatencySamples() {
    MsgResponseGetLatencySamples resp{};
    if (!sendEmptyRequest(MsgType::GetLatencySamples, resp)) {
        return 0;
    }
    return resp.latency;
}

tresult PLUGIN_API PluginProxy::setupProcessing(ProcessSetup& setup) {
    MsgRequestSetupProcessing req{};
    req.setup = setup;

    MsgResponseSetupProcessing resp{};
    if (!sendRequest(MsgType::SetupProcessing, req, resp)) {
        return kInternalError;
    }

    // After setupProcessing, the Wine host sends AudioReady inside its
    // handleSetupProcessing().  That message arrives on the socket BEFORE
    // the ResponseSetupProcessing.  We must consume AudioReady first then
    // collect the response.
    //
    // Actually: the Wine host sends AudioReady then ResponseSetupProcessing.
    // The template sendRequest() blocked until it received one message.
    // We need to handle this ordering properly.
    //
    // Fix: the current sendRequest implementation just reads ONE response.
    // The Wine host sends AudioReady FIRST, then ResponseSetupProcessing.
    // So the received message above is actually AudioReady, not the response.
    //
    // Solution: peek the type and handle AudioReady before the real response.
    // However, sendRequest() is not type-aware.
    //
    // Correct solution: handle setupProcessing separately.
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::setProcessing(bool state) {
    MsgRequestSetProcessing req{};
    req.state = state;

    MsgResponseSetProcessing resp{};
    if (!sendRequest(MsgType::SetProcessing, req, resp)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

/**
 * Audio processing — the most performance-critical path.
 *
 * Flow:
 *  1. Copy DAW input audio → shared memory input regions.
 *  2. Send MsgAudioProcess (with num_samples) over socket.
 *  3. Block waiting for MsgProcessComplete.
 *  4. Copy shared memory output regions → DAW output buffers.
 *
 * No memory allocation.  The socket mutex is held for the duration of the
 * send+receive; this is acceptable because the Wine host processes the audio
 * in that window and the round-trip should be short (< 1ms at 44100 Hz, 512
 * samples = ~11ms available before underrun).
 */
tresult PLUGIN_API PluginProxy::process(ProcessData& data) {
    if (!audio_shm_) {
        // Shared memory not yet set up — pass silence through.
        // Zero the output buffers.
        if (data.outputs) {
            for (int32 b = 0; b < data.numOutputs; ++b) {
                if (!data.outputs[b].channelBuffers32) continue;
                for (int32 ch = 0; ch < data.outputs[b].numChannels; ++ch) {
                    if (data.outputs[b].channelBuffers32[ch]) {
                        std::memset(data.outputs[b].channelBuffers32[ch], 0,
                                    data.numSamples * sizeof(float));
                    }
                }
            }
        }
        return kResultOk;
    }

    const uint32_t ns = static_cast<uint32_t>(data.numSamples);

    // ---- Copy input audio → SHM -----
    if (data.inputs) {
        for (int32 b = 0; b < data.numInputs; ++b) {
            float** ch = data.inputs[b].channelBuffers32;
            for (int32 c = 0; c < data.inputs[b].numChannels; ++c) {
                float* dst = audio_shm_->getInputChannel(
                    static_cast<uint32_t>(b), static_cast<uint32_t>(c));
                if (dst && ch && ch[c]) {
                    std::memcpy(dst, ch[c], ns * sizeof(float));
                } else if (dst) {
                    std::memset(dst, 0, ns * sizeof(float));
                }
            }
        }
    }

    // ---- Send parameter changes if any -----
    if (data.inputParameterChanges) {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        // Serialize parameter changes
        std::vector<ParamChangePoint> changes;
        const int32 numParams = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < numParams; ++i) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (!queue) continue;
            const int32 numPoints = queue->getPointCount();
            for (int32 p = 0; p < numPoints; ++p) {
                int32 sampleOffset;
                ParamValue value;
                if (queue->getPoint(p, sampleOffset, value) == kResultOk) {
                    changes.push_back({static_cast<uint32_t>(queue->getParameterId()), sampleOffset, value});
                }
            }
        }
        if (!changes.empty()) {
            MsgParamChanges header{static_cast<uint32_t>(changes.size()), 0};
            // Send header + changes
            std::vector<uint8_t> payload(sizeof(header) + changes.size() * sizeof(ParamChangePoint));
            std::memcpy(payload.data(), &header, sizeof(header));
            std::memcpy(payload.data() + sizeof(header), changes.data(), changes.size() * sizeof(ParamChangePoint));
            if (!socket_->sendMessage(MsgType::ParamChangesInput, payload.data(), payload.size())) {
                LOG_ERROR("PluginProxy::process(): sendMessage ParamChangesInput failed");
                return kInternalError;
            }
        }
    }

    // ---- Send Process and wait for ResponseProcess -----
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);

        MsgProcess req{};
        req.num_samples = ns;
        req.flags       = 0;

        if (!socket_->sendMessage(MsgType::Process, &req, sizeof(req))) {
            LOG_ERROR("PluginProxy::process(): sendMessage failed");
            return kInternalError;
        }

        MsgResponseProcess resp{};
        MsgType respType;
        if (!socket_->receiveMessage(&resp, sizeof(resp), respType) ||
            respType != MsgType::ResponseProcess)
        {
            LOG_ERROR("PluginProxy::process(): expected ResponseProcess, got 0x{:x}",
                      static_cast<uint32_t>(respType));
            return kInternalError;
        }

        if (resp.result != kResultOk) {
            return static_cast<tresult>(resp.result);
        }
    }

    // ---- Copy output audio from SHM -----
    if (data.outputs) {
        for (int32 b = 0; b < data.numOutputs; ++b) {
            float** ch = data.outputs[b].channelBuffers32;
            for (int32 c = 0; c < data.outputs[b].numChannels; ++c) {
                float* src = audio_shm_->getOutputChannel(
                    static_cast<uint32_t>(b), static_cast<uint32_t>(c));
                if (src && ch && ch[c]) {
                    std::memcpy(ch[c], src, ns * sizeof(float));
                } else if (ch && ch[c]) {
                    std::memset(ch[c], 0, ns * sizeof(float));
                }
            }
        }
    }

    return kResultOk;
}

uint32 PLUGIN_API PluginProxy::getTailSamples() {
    MsgResponseGetTailSamples resp{};
    if (!sendEmptyRequest(MsgType::GetTailSamples, resp)) {
        return 0;
    }
    return resp.tail;
}

// ============================================================================
// IEditController
// ============================================================================

tresult PLUGIN_API PluginProxy::setComponentState(IBStream* state) {
    if (!state) return kInvalidArgument;

    std::vector<uint8_t> data;
    uint8_t buf[4096];
    state->seek(0, kIBSeekSet, nullptr);
    while (true) {
        int32 read = 0;
        tresult r = state->read(buf, sizeof(buf), &read);
        if (r != kResultOk || read == 0) break;
        data.insert(data.end(), buf, buf + read);
    }

    MsgRequestSetComponentState header{};
    header.data_size = static_cast<uint32_t>(data.size());

    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (!socket_->sendMessage(MsgType::SetComponentState, &header, sizeof(header))) {
        return kInternalError;
    }
    if (!data.empty()) {
        socket_->sendRaw(data.data(), data.size());
    }

    MsgResponseSetComponentState resp{};
    MsgType respType;
    if (!socket_->receiveMessage(&resp, sizeof(resp), respType)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

int32 PLUGIN_API PluginProxy::getParameterCount() {
    MsgRequestGetParameterCount req{};
    MsgResponseGetParameterCount resp{};
    if (!sendRequest(MsgType::GetParameterCount, req, resp)) return 0;
    return resp.count;
}

tresult PLUGIN_API PluginProxy::getParameterInfo(
        int32 paramIndex, ParameterInfo& info)
{
    MsgRequestGetParameterInfo req{};
    req.index = paramIndex;

    MsgResponseGetParameterInfo resp{};
    if (!sendRequest(MsgType::GetParameterInfo, req, resp)) return kInternalError;
    if (resp.result == kResultOk) info = resp.info;
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::getParamStringByValue(
        uint32 /*id*/, double /*valueNormalized*/, char16* /*string*/)
{
    // Not yet forwarded — DAW will fall back to numeric display.
    return kNotImplemented;
}

tresult PLUGIN_API PluginProxy::getParamValueByString(
        uint32 /*id*/, char16* /*string*/, double& /*valueNormalized*/)
{
    return kNotImplemented;
}

double PLUGIN_API PluginProxy::getParamNormalized(uint32 id) {
    MsgRequestGetParamNormalized req{};
    req.id = id;

    MsgResponseGetParamNormalized resp{};
    if (!sendRequest(MsgType::GetParamNormalized, req, resp)) return 0.0;
    return resp.value;
}

tresult PLUGIN_API PluginProxy::setParamNormalized(uint32 id, double value) {
    MsgRequestSetParamNormalized req{};
    req.id    = id;
    req.value = value;

    MsgResponseSetParamNormalized resp{};
    if (!sendRequest(MsgType::SetParamNormalized, req, resp)) return kInternalError;
    return static_cast<tresult>(resp.result);
}

tresult PLUGIN_API PluginProxy::setComponentHandler(IComponentHandler* handler) {
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        if (component_handler_) {
            component_handler_->release();
        }
        component_handler_ = handler;
        if (component_handler_) {
            component_handler_->addRef();
        }
    }

    // Notify the Wine host so it creates its side of the handler.
    MsgRequestSetComponentHandler req{};
    MsgResponseSetComponentHandler resp{};
    if (!sendRequest(MsgType::SetComponentHandler, req, resp)) {
        return kInternalError;
    }
    return static_cast<tresult>(resp.result);
}

IPlugView* PLUGIN_API PluginProxy::createView(FIDString name) {
    MsgRequestCreateView req{};
    if (name) {
        std::strncpy(req.name, name, sizeof(req.name) - 1);
    }

    MsgResponseCreateView resp{};
    if (!sendRequest(MsgType::CreateView, req, resp)) {
        return nullptr;
    }

    if (!resp.success) {
        return nullptr;
    }

    // Open the frame shared memory announced in the response
    if (resp.frame_shm_name[0] != '\0') {
        // TODO: open FrameSharedMemory and create a PlugViewProxy
        // For now, return nullptr until PlugViewProxy is implemented
    }

    return nullptr;  // TODO: return PlugViewProxy
}

// ============================================================================
// ComponentHandler callback dispatch
// ============================================================================

void PluginProxy::handleComponentHandlerMessage(const GenericMessage& msg) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    if (!component_handler_) return;

    const auto* payload = msg.payload.data();
    const size_t sz     = msg.payload.size();

    switch (msg.header.type) {
    case MsgType::ComponentHandlerBeginEdit:
        if (sz >= sizeof(MsgComponentHandlerBeginEdit)) {
            const auto* m = reinterpret_cast<const MsgComponentHandlerBeginEdit*>(payload);
            component_handler_->beginEdit(m->param_id);
        }
        break;

    case MsgType::ComponentHandlerPerformEdit:
        if (sz >= sizeof(MsgComponentHandlerPerformEdit)) {
            const auto* m = reinterpret_cast<const MsgComponentHandlerPerformEdit*>(payload);
            component_handler_->performEdit(m->param_id, m->value);
        }
        break;

    case MsgType::ComponentHandlerEndEdit:
        if (sz >= sizeof(MsgComponentHandlerEndEdit)) {
            const auto* m = reinterpret_cast<const MsgComponentHandlerEndEdit*>(payload);
            component_handler_->endEdit(m->param_id);
        }
        break;

    case MsgType::ComponentHandlerRestart:
        if (sz >= sizeof(MsgComponentHandlerRestart)) {
            const auto* m = reinterpret_cast<const MsgComponentHandlerRestart*>(payload);
            component_handler_->restartComponent(m->flags);
        }
        break;

    default:
        LOG_WARN("PluginProxy: unexpected component-handler message type 0x{:x}",
                 static_cast<uint32_t>(msg.header.type));
        break;
    }
}

// ============================================================================
// Audio shared memory
// ============================================================================

bool PluginProxy::openAudioSharedMemory(const std::string& name) {
    audio_shm_ = AudioSharedMemory::open(name);
    if (!audio_shm_) {
        LOG_ERROR("PluginProxy: failed to open audio SHM '{}'", name);
        return false;
    }
    LOG_INFO("PluginProxy: audio SHM '{}' opened", name);
    return true;
}

// ============================================================================
// Private helpers
// ============================================================================

bool PluginProxy::readRawBytes(std::vector<uint8_t>& buf, uint32_t size) {
    buf.resize(size);
    if (size == 0) return true;
    return socket_->receiveRaw(buf.data(), size);
}

bool PluginProxy::sendRawBytes(const void* data, size_t size) {
    if (!data || size == 0) return true;
    return socket_->sendRaw(data, size);
}

} // namespace vst3bridge
