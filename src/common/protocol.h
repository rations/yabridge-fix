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
 * @file protocol.h
 * @brief IPC protocol between the native Linux library and the Wine host.
 *
 * All messages are sent as:
 *   [ MessageHeader (fixed 24 bytes) ][ payload (payload_size bytes) ]
 *
 * The message header carries a magic number, protocol version, message
 * type and the size of the following payload.  Both sides must validate
 * the magic and version fields before processing the payload.
 *
 * Design principles:
 *  - All request/response message structs are POD types with no pointers,
 *    strings shorter than 64 bytes are stored inline, longer data (state
 *    blobs, pixel buffers) is exchanged via POSIX shared memory.
 *  - Enum fields in structs are stored as int32_t for deterministic
 *    serialisation across compilers.
 *  - Every request has exactly one response; timeouts are handled at the
 *    socket layer.
 */

#pragma once

#include <cstdint>
#include <vector>

// Pull in VST3 types we embed in message structures.
// These are all POD structs so including them here is safe.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/gui/iplugview.h"

namespace vst3bridge {

// ============================================================================
// Protocol version
// ============================================================================

/** Increment this whenever a breaking change is made to the message layout. */
static constexpr uint32_t PROTOCOL_VERSION = 1;

// ============================================================================
// Message type enumeration
// ============================================================================

/**
 * @brief All message types exchanged between the native library and Wine host.
 *
 * Naming convention: requests are unprefixed (or prefixed "Request" in the
 * message struct); responses are prefixed "Response" in the enum and struct.
 *
 * Ranges:
 *   0x0001 – 0x00FF  Connection / lifecycle
 *   0x0100 – 0x01FF  IPluginFactory
 *   0x0200 – 0x02FF  IPluginBase (initialize / terminate)
 *   0x0300 – 0x03FF  IComponent (bus management, state)
 *   0x0400 – 0x04FF  IAudioProcessor
 *   0x0500 – 0x05FF  IEditController
 *   0x0600 – 0x06FF  IPlugView
 *   0x0700 – 0x07FF  GUI frame updates and input events
 *   0xFFFF           Error
 */
enum class MsgType : uint32_t {

    // ---- Connection ---------------------------------------------------------
    Handshake         = 0x0001,
    HandshakeResponse = 0x0002,
    Ping              = 0x0003,
    Pong              = 0x0004,
    Shutdown          = 0x0005,  ///< Native → Wine: request graceful shutdown
    ShutdownAck       = 0x0006,  ///< Wine → Native: shutdown acknowledged

    // ---- IPluginFactory -----------------------------------------------------
    RequestFactoryInfo  = 0x0100,
    ResponseFactoryInfo = 0x0101,
    RequestClassCount   = 0x0102,
    ResponseClassCount  = 0x0103,
    RequestClassInfo    = 0x0104,
    ResponseClassInfo   = 0x0105,
    RequestClassInfo2   = 0x0106,
    ResponseClassInfo2  = 0x0107,
    RequestClassInfoW   = 0x0108,
    ResponseClassInfoW  = 0x0109,
    CreateInstance      = 0x010A,
    ResponseCreateInstance = 0x010B,
    SetHostContext      = 0x010C,
    ResponseSetHostContext = 0x010D,

    // ---- IPluginBase --------------------------------------------------------
    Initialize         = 0x0200,
    ResponseInitialize = 0x0201,
    Terminate          = 0x0202,
    ResponseTerminate  = 0x0203,

    // ---- IComponent ---------------------------------------------------------
    GetControllerClassId         = 0x0300,
    ResponseGetControllerClassId = 0x0301,
    SetIoMode                    = 0x0302,
    ResponseSetIoMode            = 0x0303,
    GetBusCount                  = 0x0304,
    ResponseGetBusCount          = 0x0305,
    GetBusInfo                   = 0x0306,
    ResponseGetBusInfo           = 0x0307,
    ActivateBus                  = 0x0308,
    ResponseActivateBus          = 0x0309,
    SetActive                    = 0x030A,
    ResponseSetActive            = 0x030B,
    SetState                     = 0x030C,
    ResponseSetState             = 0x030D,
    GetState                     = 0x030E,
    ResponseGetState             = 0x030F,

    // ---- IAudioProcessor ----------------------------------------------------
    SetBusArrangements            = 0x0400,
    ResponseSetBusArrangements    = 0x0401,
    GetBusArrangement             = 0x0402,
    ResponseGetBusArrangement     = 0x0403,
    CanProcessSampleSize          = 0x0404,
    ResponseCanProcessSampleSize  = 0x0405,
    GetLatencySamples             = 0x0406,
    ResponseGetLatencySamples     = 0x0407,
    SetupProcessing               = 0x0408,
    ResponseSetupProcessing       = 0x0409,
    SetProcessing                 = 0x040A,
    ResponseSetProcessing         = 0x040B,
    Process                       = 0x040C,
    ResponseProcess               = 0x040D,
    GetTailSamples                = 0x040E,
    ResponseGetTailSamples        = 0x040F,
    /// Wine → Native: Wine is configured and ready to accept Process messages.
    /// Sent once after SetupProcessing completes, before the first Process call.
    AudioReady                    = 0x0410,
    /// Native → Wine: send a block of audio; data is in shared memory.
    AudioProcess                  = 0x0411,
    /// Wine → Native: audio processing complete; output is in shared memory.
    ProcessComplete               = 0x0412,
    /// Native → Wine: parameter changes for the upcoming audio block.
    ParamChangesInput             = 0x0413,
    /// Wine → Native: controller-side parameter changes (from GUI).
    ParamChangesOutput            = 0x0414,

    // ---- IEditController ----------------------------------------------------
    SetComponentState             = 0x0500,
    ResponseSetComponentState     = 0x0501,
    GetParameterCount             = 0x0502,
    ResponseGetParameterCount     = 0x0503,
    GetParameterInfo              = 0x0504,
    ResponseGetParameterInfo      = 0x0505,
    GetParamNormalized            = 0x0506,
    ResponseGetParamNormalized    = 0x0507,
    SetParamNormalized            = 0x0508,
    ResponseSetParamNormalized    = 0x0509,
    SetComponentHandler           = 0x050A,
    ResponseSetComponentHandler   = 0x050B,
    CreateView                    = 0x050C,
    ResponseCreateView            = 0x050D,
    /// Wine → Native: plugin GUI called IComponentHandler::beginEdit()
    ComponentHandlerBeginEdit     = 0x050E,
    /// Wine → Native: plugin GUI called IComponentHandler::performEdit()
    ComponentHandlerPerformEdit   = 0x050F,
    /// Wine → Native: plugin GUI called IComponentHandler::endEdit()
    ComponentHandlerEndEdit       = 0x0510,
    /// Wine → Native: plugin called IComponentHandler::restartComponent()
    ComponentHandlerRestart       = 0x0511,

    // ---- IPlugView ----------------------------------------------------------
    ViewAttached      = 0x0600,
    ResponseViewAttached = 0x0601,
    ViewRemoved       = 0x0602,
    ResponseViewRemoved = 0x0603,
    ViewOnWheel       = 0x0604,
    ResponseViewOnWheel = 0x0605,
    ViewGetSize       = 0x0606,
    ResponseViewGetSize = 0x0607,
    ViewOnSize        = 0x0608,
    ResponseViewOnSize = 0x0609,
    ViewCanResize     = 0x060A,
    ResponseViewCanResize = 0x060B,
    ViewCheckSizeConstraint = 0x060C,
    ResponseViewCheckSizeConstraint = 0x060D,

    // ---- GUI frame updates --------------------------------------------------
    /// Wine → Native: new frame is ready in FrameSharedMemory
    FrameUpdate = 0x0700,
    /// Native → Wine: frame has been displayed; producer can reuse the slot
    FrameAck    = 0x0701,
    /// Native → Wine: mouse / keyboard input event forwarded to plugin
    InputEvent  = 0x0702,
    /// Wine → Native: input event has been processed
    InputAck    = 0x0703,

    // ---- Errors -------------------------------------------------------------
    Error = 0xFFFF
};

// ============================================================================
// MessageHeader  (24 bytes, fixed size, sent before every payload)
// ============================================================================

/**
 * @brief Fixed-size header prepended to every message on the socket.
 *
 * Both sides validate magic and version before interpreting the payload.
 */
struct MessageHeader {
    uint32_t magic;         ///< Must equal kMagic
    uint32_t version;       ///< Must equal PROTOCOL_VERSION
    MsgType  type;          ///< Message identity
    uint32_t payload_size;  ///< Bytes of payload that follow this header
    uint64_t timestamp;     ///< Sender's monotonic clock value (for diagnostics)

    static constexpr uint32_t kMagic = 0x56535433u; // "VST3"
};
static_assert(sizeof(MessageHeader) == 24, "MessageHeader size mismatch");

// ============================================================================
// Generic message container (used server-side for receiving unknown messages)
// ============================================================================

struct GenericMessage {
    MessageHeader          header;
    std::vector<uint8_t>   payload;
};

// ============================================================================
// Connection messages
// ============================================================================

struct MsgHandshake {
    // No payload; header magic/version are sufficient
};

struct MsgResponseHandshake {
    uint32_t protocol_version;
};

// ============================================================================
// IPluginFactory messages
// ============================================================================

struct MsgRequestFactoryInfo {};

struct MsgResponseFactoryInfo {
    Steinberg::PFactoryInfo info;
};

struct MsgRequestClassCount {};

struct MsgResponseClassCount {
    int32_t count;
};

struct MsgRequestClassInfo {
    int32_t index;
};

struct MsgResponseClassInfo {
    bool success;
    Steinberg::PClassInfo info;
};

struct MsgRequestClassInfo2 {
    int32_t index;
};

struct MsgResponseClassInfo2 {
    bool success;
    Steinberg::PClassInfo2 info;
};

struct MsgRequestClassInfoW {
    int32_t index;
};

struct MsgResponseClassInfoW {
    bool success;
    Steinberg::PClassInfoW info;
};

struct MsgRequestCreateInstance {
    Steinberg::TUID cid;   ///< Class ID to instantiate
    Steinberg::TUID iid;   ///< Interface ID to return
};

struct MsgResponseCreateInstance {
    bool     success;
    uint64_t instance_id;  ///< Opaque ID used in all subsequent calls
};

struct MsgRequestSetHostContext {};

struct MsgResponseSetHostContext {
    bool success;
};

// ============================================================================
// IPluginBase messages
// ============================================================================

struct MsgRequestInitialize {
    uint64_t context;  ///< Opaque host context token (not a pointer on Wine side)
};

struct MsgResponseInitialize {
    int32_t result;  ///< tresult cast to int32
};

struct MsgRequestTerminate {};

struct MsgResponseTerminate {
    int32_t result;
};

// ============================================================================
// IComponent messages
// ============================================================================

struct MsgRequestGetControllerClassId {};

struct MsgResponseGetControllerClassId {
    bool           success;
    Steinberg::TUID classId;
};

struct MsgRequestSetIoMode {
    int32_t mode;  ///< Steinberg::IoMode
};

struct MsgResponseSetIoMode {
    int32_t result;
};

struct MsgRequestGetBusCount {
    int32_t media_type;  ///< Steinberg::MediaType (int32)
    int32_t direction;   ///< Steinberg::BusDirection (int32)
};

struct MsgResponseGetBusCount {
    int32_t count;
};

struct MsgRequestGetBusInfo {
    int32_t media_type;
    int32_t direction;
    int32_t index;
};

struct MsgResponseGetBusInfo {
    int32_t           result;
    Steinberg::BusInfo info;
};

struct MsgRequestActivateBus {
    int32_t media_type;
    int32_t direction;
    int32_t index;
    bool    state;
    uint8_t _pad[3];  ///< Explicit padding to avoid struct size surprises
};

struct MsgResponseActivateBus {
    int32_t result;
};

struct MsgRequestSetActive {
    bool state;
};

struct MsgResponseSetActive {
    int32_t result;
};

/**
 * @brief Header for SetState / GetState data transfer.
 *
 * When sending state data (preset / project), this header is followed
 * immediately by @c data_size raw bytes on the same socket.  The receiver
 * reads exactly those bytes to reconstruct the state.
 */
struct MsgRequestSetState {
    uint32_t data_size;
    // data_size raw bytes follow on the socket
};

struct MsgResponseSetState {
    int32_t result;
};

struct MsgRequestGetState {};

struct MsgResponseGetState {
    int32_t  result;
    uint32_t data_size;
    // data_size raw bytes follow on the socket when result == kResultOk
};

struct MsgRequestSetComponentState {
    uint32_t data_size;
    // data_size raw bytes follow
};

struct MsgResponseSetComponentState {
    int32_t result;
};

// ============================================================================
// IAudioProcessor messages
// ============================================================================

struct MsgRequestSetBusArrangements {
    int32_t  num_ins;
    int32_t  num_outs;
    uint64_t in_arr[8];   ///< SpeakerArrangement per input bus (up to 8)
    uint64_t out_arr[8];  ///< SpeakerArrangement per output bus (up to 8)
};

struct MsgResponseSetBusArrangements {
    int32_t result;
};

struct MsgRequestGetBusArrangement {
    int32_t direction;   ///< Steinberg::BusDirection
    int32_t bus_index;
};

struct MsgResponseGetBusArrangement {
    int32_t  result;
    uint64_t arrangement;  ///< Steinberg::SpeakerArrangement
};

struct MsgRequestCanProcessSampleSize {
    int32_t symbolic_sample_size;  ///< SymbolicSampleSizes value
};

struct MsgResponseCanProcessSampleSize {
    int32_t result;
};

struct MsgRequestGetLatencySamples {};

struct MsgResponseGetLatencySamples {
    uint32_t latency;
};

struct MsgRequestSetupProcessing {
    Steinberg::ProcessSetup setup;
};

struct MsgResponseSetupProcessing {
    int32_t result;
};

struct MsgRequestSetProcessing {
    bool state;
};

struct MsgResponseSetProcessing {
    int32_t result;
};

/**
 * @brief Sent by the Wine host once it has completed setupProcessing()
 *        and is ready to accept Process messages.
 *
 * Also carries the shared-memory name so the native side can open the
 * audio buffers.
 */
struct MsgAudioReady {
    char     shm_name[64];         ///< Null-terminated POSIX SHM name
    uint32_t input_bus_count;
    uint32_t output_bus_count;
    uint32_t input_bus_channels[8];
    uint32_t output_bus_channels[8];
};

/**
 * @brief Sent by the native side to trigger one audio block.
 *
 * Audio input data has already been written to the shared memory before
 * sending this message.  The Wine host reads SHM → processes → writes
 * output SHM → sends ProcessComplete.
 */
struct MsgProcess {
    uint32_t num_samples;
    uint32_t flags;          ///< Reserved, must be 0
};

struct MsgResponseProcess {
    int32_t result;
};

struct MsgRequestGetTailSamples {};

struct MsgResponseGetTailSamples {
    uint32_t tail;
};

// ============================================================================
// Parameter change messages
// ============================================================================

/**
 * @brief One parameter change point (sample-accurate).
 */
struct ParamChangePoint {
    uint32_t param_id;     ///< Parameter ID
    int32_t  sample_offset;///< Sample offset within the buffer
    double   value;        ///< Normalised parameter value [0.0, 1.0]
};

/**
 * @brief Header for a block of ParamChangePoint structs.
 *
 * Followed immediately by @c num_changes × sizeof(ParamChangePoint) bytes.
 */
struct MsgParamChanges {
    uint32_t num_changes;
    uint32_t _pad;
    // ParamChangePoint changes[num_changes] follow
};

// ============================================================================
// IEditController messages
// ============================================================================

struct MsgRequestGetParameterCount {};

struct MsgResponseGetParameterCount {
    int32_t count;
};

struct MsgRequestGetParameterInfo {
    int32_t index;
};

struct MsgResponseGetParameterInfo {
    int32_t result;
    Steinberg::ParameterInfo info;
};

struct MsgRequestGetParamNormalized {
    uint32_t id;
};

struct MsgResponseGetParamNormalized {
    double value;
};

struct MsgRequestSetParamNormalized {
    uint32_t id;
    double   value;
};

struct MsgResponseSetParamNormalized {
    int32_t result;
};

struct MsgRequestSetComponentHandler {};

struct MsgResponseSetComponentHandler {
    int32_t result;
};

struct MsgRequestCreateView {
    char name[64];  ///< View type identifier e.g. "editor"
};

struct MsgResponseCreateView {
    bool    success;
    uint8_t _pad[3];
    int32_t width;
    int32_t height;
    char    frame_shm_name[64];  ///< POSIX SHM name for FrameSharedMemory
};

// ---- IComponentHandler callbacks (Wine → Native) ---------------------------

struct MsgComponentHandlerBeginEdit {
    uint32_t param_id;
};

struct MsgComponentHandlerPerformEdit {
    uint32_t param_id;
    double   value;
};

struct MsgComponentHandlerEndEdit {
    uint32_t param_id;
};

struct MsgComponentHandlerRestart {
    int32_t flags;  ///< Steinberg::RestartFlags bitmask
};

// ============================================================================
// IPlugView messages
// ============================================================================

struct MsgRequestViewAttached {
    uint64_t view_id;
    uint64_t parent_window;   ///< X11 Window ID (on Linux)
    char     platform_type[32]; ///< "HWND", "X11WindowID", etc.
};

struct MsgResponseViewAttached {
    int32_t result;
};

struct MsgRequestViewRemoved {
    uint64_t view_id;
};

struct MsgResponseViewRemoved {
    int32_t result;
};

struct MsgRequestViewGetSize {
    uint64_t view_id;
};

struct MsgResponseViewGetSize {
    int32_t             result;
    Steinberg::ViewRect rect;
};

struct MsgRequestViewOnSize {
    uint64_t            view_id;
    Steinberg::ViewRect rect;
};

struct MsgResponseViewOnSize {
    int32_t result;
};

struct MsgRequestViewCanResize {
    uint64_t view_id;
};

struct MsgResponseViewCanResize {
    int32_t result;  ///< kResultTrue = can resize
};

// ============================================================================
// GUI frame update messages
// ============================================================================

/**
 * @brief Sent by the Wine host when a new frame has been written to the
 *        FrameSharedMemory.
 *
 * The dirty rectangle allows the native renderer to skip a full blit when
 * only a small region has changed.
 */
struct MsgFrameUpdate {
    uint32_t frame_id;      ///< Monotonically increasing frame counter
    uint32_t width;
    uint32_t height;
    uint32_t format;        ///< 0 = BGRA32 (only supported format currently)
    uint32_t dirty_x;
    uint32_t dirty_y;
    uint32_t dirty_width;
    uint32_t dirty_height;
};

/**
 * @brief Sent by the native side once it has consumed (displayed) a frame.
 * The Wine host may now reuse that frame slot.
 */
struct MsgFrameAck {
    uint32_t frame_id;
};

/**
 * @brief Mouse or keyboard event forwarded from the native X11 window to
 *        the Wine plugin window.
 */
struct MsgInputEvent {
    enum class Type : uint32_t {
        MouseMove    = 0,
        MouseDown    = 1,
        MouseUp      = 2,
        MouseWheel   = 3,
        KeyDown      = 4,
        KeyUp        = 5,
        FocusIn      = 6,
        FocusOut     = 7
    };

    Type     type;
    uint64_t view_id;
    int32_t  x;        ///< Mouse X in plugin coords, or key code
    int32_t  y;        ///< Mouse Y in plugin coords, or modifier mask
    int32_t  button;   ///< Mouse button (1=left, 2=middle, 3=right) or char
    float    delta;    ///< Scroll wheel delta
};

struct MsgInputAck {
    bool handled;
};

// ============================================================================
// Shared-memory layout descriptors
//
// These structs describe the layout at the beginning of the respective
// shared-memory region.  All code on both sides uses these descriptors
// to compute channel-buffer offsets.
// ============================================================================

/**
 * @brief Layout header at the start of the audio shared-memory region.
 *
 * Audio channels are stored as contiguous float (or double) arrays.
 * Their offsets from the start of the SHM region are stored in
 * input_offsets[] / output_offsets[].
 */
struct AudioBufferLayout {
    static constexpr uint32_t kMaxBuses           = 8;
    static constexpr uint32_t kMaxChannelsPerBus  = 16;
    static constexpr uint32_t kMaxTotalChannels   = 64;
    static constexpr uint32_t kMaxSamples         = 8192;

    uint32_t num_input_buses;
    uint32_t num_output_buses;
    uint32_t num_samples;
    uint32_t sample_size;   ///< 4 = float32, 8 = float64

    uint32_t input_bus_channels[kMaxBuses];    ///< Channel count per input bus
    uint32_t output_bus_channels[kMaxBuses];   ///< Channel count per output bus

    /// Map from (bus, channel) → total channel index within the flat array.
    /// 0xFFFFFFFF means the slot is not assigned.
    uint32_t input_bus_channel_index[kMaxBuses][kMaxChannelsPerBus];
    uint32_t output_bus_channel_index[kMaxBuses][kMaxChannelsPerBus];

    /// Byte offsets from start of SHM for each channel's sample buffer.
    uint64_t input_offsets[kMaxTotalChannels];
    uint64_t output_offsets[kMaxTotalChannels];
};

/**
 * @brief Layout header at the start of the frame shared-memory region.
 *
 * A triple-buffered ring is stored after this header; slot pixel data is
 * accessed via getSlotOffset() as implemented in FrameSharedMemory.
 */
struct FrameBufferLayout {
    static constexpr uint32_t kMaxWidth      = 4096;
    static constexpr uint32_t kMaxHeight     = 4096;
    static constexpr uint32_t kBytesPerPixel = 4;  ///< BGRA32

    uint32_t max_width;
    uint32_t max_height;
    uint32_t format;        ///< 0 = BGRA32
    uint32_t _pad;
};

} // namespace vst3bridge
