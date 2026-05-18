// yabridge: GDI frame capture shared memory implementation.
// Uses POSIX shm_open/mmap. On the Wine side, WIN32 macros are pushed/popped
// so that the Linux POSIX headers are included cleanly.

#pragma push_macro("WIN32")
#pragma push_macro("_WIN32")
#undef WIN32
#undef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#pragma pop_macro("_WIN32")
#pragma pop_macro("WIN32")

#include <cstdio>
#include <cstdlib>
#include <new>

#include "frame_shm.h"

namespace yabridge {

static constexpr uint32_t kBytesPerPixel = 4;  // BGRA

std::unique_ptr<FrameSharedMemory> FrameSharedMemory::create(uint32_t max_w,
                                                              uint32_t max_h) {
    auto self = std::unique_ptr<FrameSharedMemory>(new FrameSharedMemory());

    char buf[64];
    snprintf(buf, sizeof(buf), "/yabridge_frame_%d_%u", (int)getpid(),
             (unsigned)rand());
    self->name_ = buf;

    self->pixel_slot_size_ =
        static_cast<size_t>(max_w) * max_h * kBytesPerPixel;
    self->size_ = sizeof(Ring) + Ring::kSlots * self->pixel_slot_size_;

    self->fd_ =
        shm_open(self->name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (self->fd_ < 0) return nullptr;

    if (ftruncate(self->fd_, (off_t)self->size_) < 0) {
        shm_unlink(self->name_.c_str());
        close(self->fd_);
        return nullptr;
    }

    self->data_ = mmap(nullptr, self->size_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, self->fd_, 0);
    if (self->data_ == MAP_FAILED) {
        self->data_ = nullptr;
        shm_unlink(self->name_.c_str());
        close(self->fd_);
        return nullptr;
    }

    self->owner_ = true;
    self->ring_ = new (self->data_) Ring{};
    self->pixels_ = static_cast<uint8_t*>(self->data_) + sizeof(Ring);

    self->ring_->write_idx.store(0, std::memory_order_relaxed);
    self->ring_->read_idx.store(0, std::memory_order_relaxed);
    self->ring_->frame_count.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < Ring::kSlots; ++i) {
        new (&self->ring_->slots[i]) Slot{};
        self->ring_->slots[i].state.store(0, std::memory_order_relaxed);
        self->ring_->slots[i].width = 0;
        self->ring_->slots[i].height = 0;
        self->ring_->slots[i].stride = max_w * kBytesPerPixel;
    }
    self->ring_->input_write.store(0, std::memory_order_relaxed);
    self->ring_->input_read.store(0, std::memory_order_relaxed);

    return self;
}

std::unique_ptr<FrameSharedMemory> FrameSharedMemory::open(
    const std::string& name) {
    auto self = std::unique_ptr<FrameSharedMemory>(new FrameSharedMemory());
    self->name_ = name;

    self->fd_ = shm_open(name.c_str(), O_RDWR, 0600);
    if (self->fd_ < 0) return nullptr;

    struct stat st{};
    if (fstat(self->fd_, &st) < 0) {
        close(self->fd_);
        return nullptr;
    }
    self->size_ = (size_t)st.st_size;
    if (self->size_ < sizeof(Ring)) {
        close(self->fd_);
        return nullptr;
    }

    self->data_ = mmap(nullptr, self->size_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, self->fd_, 0);
    if (self->data_ == MAP_FAILED) {
        self->data_ = nullptr;
        close(self->fd_);
        return nullptr;
    }

    self->ring_ = static_cast<Ring*>(self->data_);
    self->pixels_ = static_cast<uint8_t*>(self->data_) + sizeof(Ring);
    self->pixel_slot_size_ =
        (self->size_ - sizeof(Ring)) / Ring::kSlots;

    return self;
}

FrameSharedMemory::~FrameSharedMemory() noexcept {
    if (data_) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (owner_) {
        shm_unlink(name_.c_str());
    }
}

uint8_t* FrameSharedMemory::beginWrite(uint32_t w, uint32_t h) noexcept {
    if (!ring_ || pixel_slot_size_ == 0) return nullptr;
    if (static_cast<size_t>(w) * h * kBytesPerPixel > pixel_slot_size_)
        return nullptr;

    const uint32_t slot = ring_->write_idx.load(std::memory_order_relaxed);
    const uint32_t next = (slot + 1) % Ring::kSlots;

    ring_->slots[slot].state.store(1u, std::memory_order_relaxed);
    ring_->slots[slot].width = w;
    ring_->slots[slot].height = h;
    ring_->slots[slot].stride = w * kBytesPerPixel;
    ring_->write_idx.store(next, std::memory_order_relaxed);
    current_slot_ = (int)slot;

    return pixels_ + slot * pixel_slot_size_;
}

void FrameSharedMemory::endWrite() noexcept {
    if (current_slot_ < 0) return;
    ring_->slots[current_slot_].state.store(2u, std::memory_order_release);
    ring_->frame_count.fetch_add(1u, std::memory_order_relaxed);
    current_slot_ = -1;
}

const uint8_t* FrameSharedMemory::beginRead(uint32_t& w, uint32_t& h,
                                             uint32_t& stride) noexcept {
    if (!ring_) return nullptr;

    const uint32_t widx = ring_->write_idx.load(std::memory_order_acquire);
    for (uint32_t off = 1; off <= Ring::kSlots; ++off) {
        const uint32_t slot = (widx + Ring::kSlots - off) % Ring::kSlots;
        uint32_t expected = 2u;
        if (ring_->slots[slot].state.compare_exchange_strong(
                expected, 3u, std::memory_order_acquire)) {
            current_slot_ = (int)slot;
            w = ring_->slots[slot].width;
            h = ring_->slots[slot].height;
            stride = ring_->slots[slot].stride;
            return pixels_ + slot * pixel_slot_size_;
        }
    }
    return nullptr;
}

void FrameSharedMemory::endRead() noexcept {
    if (current_slot_ < 0) return;
    ring_->slots[current_slot_].state.store(0u, std::memory_order_release);
    current_slot_ = -1;
}

bool FrameSharedMemory::writeInput(const InputEvent& ev) noexcept {
    if (!ring_) return false;
    const uint32_t wr = ring_->input_write.load(std::memory_order_relaxed);
    const uint32_t rd = ring_->input_read.load(std::memory_order_acquire);
    const uint32_t next = (wr + 1) % Ring::kInputSlots;
    if (next == rd) return false;  // ring full, drop event
    ring_->input_slots[wr] = ev;
    ring_->input_write.store(next, std::memory_order_release);
    return true;
}

bool FrameSharedMemory::readInput(InputEvent& ev) noexcept {
    if (!ring_) return false;
    const uint32_t rd = ring_->input_read.load(std::memory_order_relaxed);
    const uint32_t wr = ring_->input_write.load(std::memory_order_acquire);
    if (rd == wr) return false;  // ring empty
    ev = ring_->input_slots[rd];
    ring_->input_read.store((rd + 1) % Ring::kInputSlots,
                            std::memory_order_release);
    return true;
}

}  // namespace yabridge
