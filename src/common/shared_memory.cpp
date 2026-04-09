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
 * @file shared_memory.cpp
 * @brief POSIX shared memory wrappers for audio and GUI frame exchange.
 */

#include "shared_memory.h"
#include "logger.h"

#ifdef BUILDING_WINE_HOST
// Wine host: Wine implements posix shm functions natively
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
// IMPORTANT: DO NOT include sys/socket.h anywhere in Wine host code!
#include <windows.h>
#elif defined(_WIN32)
#include <windows.h>
#else
// Native Linux build
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace vst3bridge {

// ============================================================================
// SharedMemory
// ============================================================================

SharedMemory::SharedMemory(const std::string& name, size_t size, bool create)
    : name_(name), size_(size), is_owner_(create)
{
    if (create) {
        // Remove stale segment and create fresh
        ::shm_unlink(name.c_str());
        fd_ = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) {
            LOG_ERROR("SharedMemory: shm_open create '{}' failed: errno {}",
                      name, errno);
            return;
        }
        if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            LOG_ERROR("SharedMemory: ftruncate '{}' to {} failed: errno {}",
                      name, size, errno);
            ::close(fd_);
            fd_ = -1;
            return;
        }
    } else {
        // Open existing segment; if size == 0 query from OS
        fd_ = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd_ < 0) {
            LOG_ERROR("SharedMemory: shm_open open '{}' failed: errno {}",
                      name, errno);
            return;
        }
        if (size == 0) {
            struct stat st{};
            if (::fstat(fd_, &st) == 0) {
                size_  = static_cast<size_t>(st.st_size);
                size   = size_;
            }
        }
    }

    if (size == 0) {
        LOG_ERROR("SharedMemory: zero-size mapping requested for '{}'", name);
        ::close(fd_);
        fd_ = -1;
        return;
    }

    data_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        LOG_ERROR("SharedMemory: mmap '{}' ({} bytes) failed: errno {}",
                  name, size, errno);
        data_ = nullptr;
        ::close(fd_);
        fd_ = -1;
    }
}

SharedMemory::~SharedMemory() {
    if (data_ != nullptr) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (is_owner_ && !name_.empty()) {
        ::shm_unlink(name_.c_str());
    }
}

void SharedMemory::unlink() {
    if (!name_.empty()) {
        ::shm_unlink(name_.c_str());
    }
}

std::string SharedMemory::generateName(const std::string& prefix) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    return prefix + "_" + std::to_string(dist(gen));
}

// ============================================================================
// AudioSharedMemory helpers
// ============================================================================

/**
 * Compute the total SHM size needed for @p max_channels input channels,
 * @p max_channels output channels, and @p max_samples samples per channel.
 */
static size_t computeAudioShmSize(uint32_t max_channels,
                                   uint32_t max_samples) noexcept
{
    // layout header + (input + output) × max_channels × max_samples × float
    return sizeof(AudioBufferLayout) +
           2u * max_channels * max_samples * sizeof(float);
}

// ============================================================================
// AudioSharedMemory — create path (is_host == true means this side owns the SHM)
// ============================================================================

AudioSharedMemory::AudioSharedMemory(uint32_t max_channels,
                                     uint32_t max_samples,
                                     bool     is_host)
    : max_channels_(max_channels)
    , max_samples_ (max_samples)
    , is_host_     (is_host)
{
    if (max_channels == 0 || max_samples == 0) {
        LOG_ERROR("AudioSharedMemory: invalid dimensions {}/{}", max_channels, max_samples);
        return;
    }

    const size_t total = computeAudioShmSize(max_channels, max_samples);
    const std::string name = SharedMemory::generateName("/vst3bridge_audio");

    shm_ = std::make_unique<SharedMemory>(name, total, is_host);
    if (!shm_->isValid()) {
        LOG_ERROR("AudioSharedMemory: failed to create SHM '{}'", name);
        shm_.reset();
        return;
    }

    // Zero the entire region
    std::memset(shm_->getData(), 0, total);

    // Initialise layout
    auto* layout = getLayout();
    layout->num_input_buses  = 0;
    layout->num_output_buses = 0;
    layout->num_samples      = max_samples;
    layout->sample_size      = sizeof(float);

    // Mark all bus/channel indices as unused
    for (uint32_t b = 0; b < AudioBufferLayout::kMaxBuses; ++b) {
        layout->input_bus_channels[b]  = 0;
        layout->output_bus_channels[b] = 0;
        for (uint32_t c = 0; c < AudioBufferLayout::kMaxChannelsPerBus; ++c) {
            layout->input_bus_channel_index[b][c]  = 0xFFFFFFFFu;
            layout->output_bus_channel_index[b][c] = 0xFFFFFFFFu;
        }
    }

    // Pre-compute offsets for the maximum supported channel count.
    // Data layout: [AudioBufferLayout][in_ch_0][in_ch_1]...[out_ch_0][out_ch_1]...
    const size_t chanBytes   = max_samples * sizeof(float);
    const size_t dataBase    = sizeof(AudioBufferLayout);

    for (uint32_t i = 0; i < AudioBufferLayout::kMaxTotalChannels; ++i) {
        layout->input_offsets[i]  = dataBase +  i      * chanBytes;
        layout->output_offsets[i] = dataBase + (AudioBufferLayout::kMaxTotalChannels + i) * chanBytes;
    }

    LOG_DEBUG("AudioSharedMemory: created '{}' ({} bytes, {}ch × {}samp)",
              name, total, max_channels, max_samples);
}

AudioSharedMemory::~AudioSharedMemory() = default;

// ============================================================================
// Channel buffer accessors
// ============================================================================

float* AudioSharedMemory::getInputChannel(uint32_t bus, uint32_t channel) const {
    auto* layout = getLayout();
    if (!layout) return nullptr;
    if (bus >= AudioBufferLayout::kMaxBuses ||
        channel >= AudioBufferLayout::kMaxChannelsPerBus)
    {
        return nullptr;
    }

    const uint32_t idx = layout->input_bus_channel_index[bus][channel];
    if (idx == 0xFFFFFFFFu) return nullptr;

    auto* base = static_cast<uint8_t*>(shm_->getData());
    return reinterpret_cast<float*>(base + layout->input_offsets[idx]);
}

float* AudioSharedMemory::getOutputChannel(uint32_t bus, uint32_t channel) const {
    auto* layout = getLayout();
    if (!layout) return nullptr;
    if (bus >= AudioBufferLayout::kMaxBuses ||
        channel >= AudioBufferLayout::kMaxChannelsPerBus)
    {
        return nullptr;
    }

    const uint32_t idx = layout->output_bus_channel_index[bus][channel];
    if (idx == 0xFFFFFFFFu) return nullptr;

    auto* base = static_cast<uint8_t*>(shm_->getData());
    return reinterpret_cast<float*>(base + layout->output_offsets[idx]);
}

// ============================================================================
// setBusConfiguration
// ============================================================================

void AudioSharedMemory::setBusConfiguration(
        const uint32_t* input_buses,  uint32_t num_input_buses,
        const uint32_t* output_buses, uint32_t num_output_buses)
{
    auto* layout = getLayout();
    if (!layout) {
        LOG_ERROR("AudioSharedMemory::setBusConfiguration: no layout");
        return;
    }

    // Clamp to maximum supported buses
    num_input_buses  = std::min(num_input_buses,  AudioBufferLayout::kMaxBuses);
    num_output_buses = std::min(num_output_buses, AudioBufferLayout::kMaxBuses);

    layout->num_input_buses  = num_input_buses;
    layout->num_output_buses = num_output_buses;

    // Reset all channel indices to "not assigned"
    for (uint32_t b = 0; b < AudioBufferLayout::kMaxBuses; ++b) {
        layout->input_bus_channels[b]  = 0;
        layout->output_bus_channels[b] = 0;
        for (uint32_t c = 0; c < AudioBufferLayout::kMaxChannelsPerBus; ++c) {
            layout->input_bus_channel_index[b][c]  = 0xFFFFFFFFu;
            layout->output_bus_channel_index[b][c] = 0xFFFFFFFFu;
        }
    }

    // Assign flat channel indices for input buses
    uint32_t flatIdx = 0;
    for (uint32_t b = 0; b < num_input_buses; ++b) {
        const uint32_t nCh = input_buses
            ? std::min(input_buses[b], AudioBufferLayout::kMaxChannelsPerBus)
            : 2u;  // default stereo
        layout->input_bus_channels[b] = nCh;
        for (uint32_t c = 0; c < nCh; ++c) {
            if (flatIdx < AudioBufferLayout::kMaxTotalChannels) {
                layout->input_bus_channel_index[b][c] = flatIdx++;
            }
        }
    }

    // Assign flat channel indices for output buses
    flatIdx = 0;
    for (uint32_t b = 0; b < num_output_buses; ++b) {
        const uint32_t nCh = output_buses
            ? std::min(output_buses[b], AudioBufferLayout::kMaxChannelsPerBus)
            : 2u;
        layout->output_bus_channels[b] = nCh;
        for (uint32_t c = 0; c < nCh; ++c) {
            if (flatIdx < AudioBufferLayout::kMaxTotalChannels) {
                layout->output_bus_channel_index[b][c] = flatIdx++;
            }
        }
    }

    LOG_DEBUG("AudioSharedMemory: configured {} input buses, {} output buses",
              num_input_buses, num_output_buses);
}

// ============================================================================
// AudioSharedMemory::open (client/opener side)
// ============================================================================

std::unique_ptr<AudioSharedMemory> AudioSharedMemory::open(const std::string& name) {
    // Open with size = 0 so SharedMemory queries the OS
    auto shm = std::make_unique<SharedMemory>(name, 0, /*create=*/false);
    if (!shm->isValid()) {
        LOG_ERROR("AudioSharedMemory::open: cannot open '{}'", name);
        return nullptr;
    }

    if (shm->getSize() < sizeof(AudioBufferLayout)) {
        LOG_ERROR("AudioSharedMemory::open: '{}' too small ({})", name, shm->getSize());
        return nullptr;
    }

    // Read dimensions from the layout
    const auto* layout = static_cast<const AudioBufferLayout*>(shm->getData());
    const uint32_t maxCh   = AudioBufferLayout::kMaxTotalChannels;
    const uint32_t maxSamp = layout->num_samples > 0 ? layout->num_samples
                                                      : AudioBufferLayout::kMaxSamples;

    // Build a wrapper that just holds the shm_ without re-opening
    // Use private ctor then swap the shm_ pointer
    auto result = std::unique_ptr<AudioSharedMemory>(
        new AudioSharedMemory(maxCh, maxSamp, /*is_host=*/false));

    // Replace the freshly-created (and incorrectly-sized) shm with the real one
    result->shm_ = std::move(shm);

    LOG_DEBUG("AudioSharedMemory::open: '{}' opened ({} bytes)", name, result->shm_->getSize());
    return result;
}

// ============================================================================
// FrameSharedMemory
// ============================================================================

FrameSharedMemory::FrameSharedMemory(uint32_t max_width,
                                     uint32_t max_height,
                                     bool     is_producer)
    : max_width_    (max_width)
    , max_height_   (max_height)
    , is_producer_  (is_producer)
    , ring_         (nullptr)
    , pixel_data_   (nullptr)
    , current_slot_ (-1)
{
    // Each slot: FrameSlot header + pixel data
    slot_pixel_size_ = static_cast<size_t>(max_width) * max_height * FrameBufferLayout::kBytesPerPixel;
    const size_t slotTotal  = sizeof(FrameSlot) + slot_pixel_size_;
    const size_t totalSize  = sizeof(RingBuffer) + RingBuffer::kNumSlots * slotTotal;

    const std::string name = SharedMemory::generateName("/vst3bridge_frame");

    shm_ = std::make_unique<SharedMemory>(name, totalSize, is_producer);
    if (!shm_->isValid()) {
        LOG_ERROR("FrameSharedMemory: failed to create SHM '{}'", name);
        shm_.reset();
        return;
    }

    auto* base  = static_cast<uint8_t*>(shm_->getData());
    ring_       = reinterpret_cast<RingBuffer*>(base);
    pixel_data_ = base + sizeof(RingBuffer);

    if (is_producer) {
        // Initialise ring buffer header
        ring_->write_index.store(0, std::memory_order_relaxed);
        ring_->read_index.store(0, std::memory_order_relaxed);
        ring_->frame_counter.store(0, std::memory_order_relaxed);

        for (uint32_t i = 0; i < RingBuffer::kNumSlots; ++i) {
            ring_->slots[i].state.store(0, std::memory_order_relaxed);
            ring_->slots[i].width  = 0;
            ring_->slots[i].height = 0;
            ring_->slots[i].stride = max_width * FrameBufferLayout::kBytesPerPixel;
        }
    }

    LOG_DEBUG("FrameSharedMemory: '{}' {} ({} x {} px)",
              name, is_producer ? "created" : "opened", max_width, max_height);
}

FrameSharedMemory::~FrameSharedMemory() = default;

size_t FrameSharedMemory::getSlotOffset(uint32_t slot) const noexcept {
    return static_cast<size_t>(slot) * (sizeof(FrameSlot) + slot_pixel_size_);
}

// ---- Producer side ----------------------------------------------------------

uint8_t* FrameSharedMemory::beginWrite(uint32_t width, uint32_t height) {
    if (!is_producer_ || !ring_) return nullptr;
    if (width > max_width_ || height > max_height_) return nullptr;

    // Select the next slot in round-robin fashion, overwriting old frames
    // if the consumer is slow (we prefer freshness over completeness).
    uint32_t slot = ring_->write_index.load(std::memory_order_relaxed);

    // Advance to the next slot so we never write into the one the consumer
    // might be reading.
    uint32_t next = (slot + 1) % RingBuffer::kNumSlots;

    // Claim by setting state 0 → 1 (writing).  If the slot is mid-read (2)
    // we still overwrite — the consumer will just skip it.
    ring_->slots[slot].state.store(1u, std::memory_order_relaxed);
    ring_->slots[slot].width  = width;
    ring_->slots[slot].height = height;
    ring_->slots[slot].stride = width * FrameBufferLayout::kBytesPerPixel;

    ring_->write_index.store(next, std::memory_order_relaxed);
    current_slot_ = static_cast<int>(slot);

    return pixel_data_ + getSlotOffset(slot) + sizeof(FrameSlot);
}

void FrameSharedMemory::endWrite() {
    if (current_slot_ < 0) return;
    // Mark ready (1 → 2)
    ring_->slots[current_slot_].state.store(2u, std::memory_order_release);
    ring_->frame_counter.fetch_add(1u, std::memory_order_relaxed);
    current_slot_ = -1;
}

// ---- Consumer side ----------------------------------------------------------

const uint8_t* FrameSharedMemory::beginRead(
        uint32_t& width, uint32_t& height, uint32_t& stride)
{
    if (is_producer_ || !ring_) return nullptr;

    // Find the most recent ready slot
    // Walk backwards from (write_index - 1) to find state == 2
    uint32_t writeIdx = ring_->write_index.load(std::memory_order_acquire);

    for (uint32_t offset = 1; offset <= RingBuffer::kNumSlots; ++offset) {
        uint32_t slot = (writeIdx + RingBuffer::kNumSlots - offset) % RingBuffer::kNumSlots;
        uint32_t expected = 2u;
        if (ring_->slots[slot].state.compare_exchange_strong(
                expected, 3u,  // 3 = being read
                std::memory_order_acquire))
        {
            current_slot_ = static_cast<int>(slot);
            width  = ring_->slots[slot].width;
            height = ring_->slots[slot].height;
            stride = ring_->slots[slot].stride;
            ring_->read_index.store(slot, std::memory_order_relaxed);
            return pixel_data_ + getSlotOffset(slot) + sizeof(FrameSlot);
        }
    }
    return nullptr;
}

void FrameSharedMemory::endRead() {
    if (current_slot_ < 0) return;
    // Release the slot back to empty (0)
    ring_->slots[current_slot_].state.store(0u, std::memory_order_release);
    current_slot_ = -1;
}

bool FrameSharedMemory::hasNewFrame() const {
    if (!ring_) return false;
    for (uint32_t i = 0; i < RingBuffer::kNumSlots; ++i) {
        if (ring_->slots[i].state.load(std::memory_order_acquire) == 2u) {
            return true;
        }
    }
    return false;
}

bool FrameSharedMemory::waitForFrame(int timeout_ms) const {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);

    while (true) {
        if (hasNewFrame()) return true;
        if (timeout_ms >= 0 &&
            std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        ::usleep(500);  // 0.5 ms poll interval
    }
}

std::unique_ptr<FrameSharedMemory> FrameSharedMemory::open(const std::string& name) {
    auto shm = std::make_unique<SharedMemory>(name, 0, /*create=*/false);
    if (!shm->isValid()) {
        LOG_ERROR("FrameSharedMemory::open: cannot open '{}'", name);
        return nullptr;
    }

    if (shm->getSize() < sizeof(RingBuffer)) {
        LOG_ERROR("FrameSharedMemory::open: '{}' too small ({})", name, shm->getSize());
        return nullptr;
    }

    // Read width/height from the first slot
    const auto* base  = static_cast<const uint8_t*>(shm->getData());
    const auto* ring  = reinterpret_cast<const RingBuffer*>(base);
    const uint32_t stride = ring->slots[0].stride;
    const uint32_t w      = stride / FrameBufferLayout::kBytesPerPixel;
    const uint32_t h      = static_cast<uint32_t>(
        (shm->getSize() - sizeof(RingBuffer)) /
        (RingBuffer::kNumSlots * (sizeof(FrameSlot) + static_cast<size_t>(stride) * 1)));
    // Provide a reasonable default if we can't derive the height
    const uint32_t maxW = (w > 0 && w <= FrameBufferLayout::kMaxWidth)   ? w   : 1920u;
    const uint32_t maxH = (h > 0 && h <= FrameBufferLayout::kMaxHeight)  ? h   : 1080u;

    auto result = std::unique_ptr<FrameSharedMemory>(
        new FrameSharedMemory(maxW, maxH, /*is_producer=*/false));

    result->shm_        = std::move(shm);
    result->ring_       = reinterpret_cast<RingBuffer*>(result->shm_->getData());
    result->pixel_data_ = static_cast<uint8_t*>(result->shm_->getData()) + sizeof(RingBuffer);

    LOG_DEBUG("FrameSharedMemory::open: '{}' opened ({}x{})", name, maxW, maxH);
    return result;
}

} // namespace vst3bridge
