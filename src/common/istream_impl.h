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
 * @file istream_impl.h
 * @brief Memory-backed IBStream implementation.
 *
 * MemoryStream implements the Steinberg::IBStream interface over a
 * contiguous byte buffer held in a std::vector.  It is used on both the
 * native and Wine sides to serialise / deserialise plugin state data:
 *
 *  - On the Wine side, a writable MemoryStream is passed to
 *    IComponent::getState(); the plugin fills it; the resulting buffer
 *    is then sent over the socket.
 *
 *  - On the Wine side, a read-only MemoryStream is constructed from
 *    bytes received over the socket and passed to IComponent::setState().
 *
 *  - On the native side, PluginProxy::getState() receives bytes over the
 *    socket and writes them into the IBStream provided by the DAW.
 *    PluginProxy::setState() reads bytes from the DAW-provided IBStream
 *    into a local buffer and sends them over the socket.
 *
 * Thread-safety: MemoryStream is NOT thread-safe.  Do not share a single
 * instance across threads without external locking.
 */

#pragma once

#include "pluginterfaces/base/ibstream.h"
#include <cstring>
#include <vector>

namespace vst3bridge {

/**
 * @brief Memory-backed IBStream.
 *
 * Implements Steinberg::IBStream over a std::vector<uint8_t>.
 * The stream cursor is a 64-bit integer so very large state blobs are
 * supported without overflow.
 */
class MemoryStream final : public Steinberg::IBStream {
public:
    // ---- Construction -------------------------------------------------------

    /** Create an empty writable stream. */
    MemoryStream() noexcept : refCount_(1) {}

    /** Create a read-only stream pre-loaded with @p data. */
    explicit MemoryStream(const std::vector<uint8_t>& data)
        : refCount_(1), buffer_(data), pos_(0) {}

    /** Create a read-only stream from a raw byte pointer. */
    MemoryStream(const void* data, Steinberg::int32 size)
        : refCount_(1), pos_(0) {
        if (data && size > 0) {
            buffer_.assign(
                static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + size);
        }
    }

    // ---- FUnknown -----------------------------------------------------------

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override;
    Steinberg::uint32 PLUGIN_API release() override;

    // ---- IBStream -----------------------------------------------------------

    /**
     * @brief Read @p numBytes from the current position.
     * @return kResultOk even if fewer bytes were available; *numBytesRead
     *         reflects the actual count.  Returns kResultFalse only on
     *         internal error (e.g. null buffer pointer).
     */
    Steinberg::tresult PLUGIN_API read(
            void* buffer,
            Steinberg::int32 numBytes,
            Steinberg::int32* numBytesRead) override
    {
        if (!buffer || numBytes < 0) return Steinberg::kInvalidArgument;

        const Steinberg::int64 available =
            static_cast<Steinberg::int64>(buffer_.size()) - pos_;
        const Steinberg::int32 toRead =
            static_cast<Steinberg::int32>(
                std::min<Steinberg::int64>(numBytes, available));

        if (toRead > 0) {
            std::memcpy(buffer, buffer_.data() + pos_, toRead);
            pos_ += toRead;
        }

        if (numBytesRead) *numBytesRead = toRead;
        return Steinberg::kResultOk;
    }

    /**
     * @brief Write @p numBytes to the stream at the current position,
     *        growing the buffer as necessary.
     */
    Steinberg::tresult PLUGIN_API write(
            void* buffer,
            Steinberg::int32 numBytes,
            Steinberg::int32* numBytesWritten) override
    {
        if (!buffer || numBytes < 0) return Steinberg::kInvalidArgument;
        if (numBytes == 0) {
            if (numBytesWritten) *numBytesWritten = 0;
            return Steinberg::kResultOk;
        }

        const Steinberg::int64 newEnd = pos_ + numBytes;
        if (newEnd > static_cast<Steinberg::int64>(buffer_.size())) {
            buffer_.resize(static_cast<size_t>(newEnd));
        }

        std::memcpy(buffer_.data() + pos_, buffer, numBytes);
        pos_ += numBytes;

        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultOk;
    }

    /**
     * @brief Seek to a new position.
     * @param mode kIBSeekSet (absolute), kIBSeekCur (relative), or
     *             kIBSeekEnd (relative to end).
     */
    Steinberg::tresult PLUGIN_API seek(
            Steinberg::int64 pos,
            Steinberg::int32 mode,
            Steinberg::int64* result) override
    {
        Steinberg::int64 newPos = 0;

        switch (mode) {
            case 0: // kIBSeekSet
                pos = pos_;
                break;
            case 1: // kIBSeekCur
                pos = pos_;
                break;
            case 2: // kIBSeekEnd
                pos = static_cast<Steinberg::int64>(buffer_.size());
                break;
            default:
                return Steinberg::kInvalidArgument;
        }

        if (newPos < 0) {
            return Steinberg::kInvalidArgument;
        }

        pos_ = newPos;

        if (result) *result = pos_;
        return Steinberg::kResultOk;
    }

    /** @return kResultOk with *pos set to the current byte offset. */
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = pos_;
        return Steinberg::kResultOk;
    }

    // ---- Buffer access (bridge-internal, not part of IBStream) -------------

    /** @return Const reference to the underlying buffer. */
    const std::vector<uint8_t>& buffer() const noexcept { return buffer_; }

    /** @return Size of data written so far (or total buffer size). */
    size_t size() const noexcept { return buffer_.size(); }

    /** @return Current stream position (bytes from start). */
    Steinberg::int64 position() const noexcept { return pos_; }

    /** Reset position to start of stream (does not clear data). */
    void rewind() noexcept { pos_ = 0; }

    /** Clear all data and reset position. */
    void clear() noexcept {
        buffer_.clear();
        pos_ = 0;
    }

    // ---- FUnknown iid -------------------------------------------------------

    static const Steinberg::FUID iid;

private:
    Steinberg::int32         refCount_{1};
    std::vector<uint8_t>     buffer_;
    Steinberg::int64         pos_{0};
};

// ---- FUnknown implementations -----------------------------------------------

// Keep iid distinct from IBStream::iid so bridged code can tell them apart.
inline const Steinberg::FUID MemoryStream::iid(
        0xF3C1AAEEu, 0x4B7D5F29u, 0xC8E64A3Bu, 0x7D9F2B1Eu);

inline Steinberg::uint32 PLUGIN_API MemoryStream::addRef() {
    return static_cast<Steinberg::uint32>(++refCount_);
}

inline Steinberg::uint32 PLUGIN_API MemoryStream::release() {
    Steinberg::uint32 r = static_cast<Steinberg::uint32>(--refCount_);
    if (r == 0) delete this;
    return r;
}

inline Steinberg::tresult PLUGIN_API MemoryStream::queryInterface(
        const Steinberg::TUID _iid, void** obj)
{
    if (!obj) return Steinberg::kInvalidArgument;
    Steinberg::FUID requested = Steinberg::FUID::fromTUID(_iid);
    if (requested == Steinberg::FUnknown::iid ||
        requested == Steinberg::IBStream::iid ||
        requested == MemoryStream::iid)
    {
        *obj = static_cast<Steinberg::IBStream*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

} // namespace vst3bridge
