// yabridge: a Wine plugin bridge - GDI frame capture shared memory
// Triple-buffer POSIX SHM ring for transferring Wine BGRA frames to native.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace yabridge {

class FrameSharedMemory {
   public:
    // Slot metadata lives inside the Ring struct; pixel data follows Ring.
    struct Slot {
        std::atomic<uint32_t> state;  // 0=empty 1=writing 2=ready 3=reading
        uint32_t width;
        uint32_t height;
        uint32_t stride;  // bytes per row
    };

    // Mouse/keyboard input event forwarded from the native render_window_ to
    // the Wine-side HWND.  type values:
    //   0=none 1=move 2=ldown 3=lup 4=rdown 5=rup 6=mdown 7=mup 8=wheel
    struct InputEvent {
        int16_t x;
        int16_t y;
        int16_t wheel_delta;
        uint8_t type;
        uint8_t buttons;  // button-state mask (future use)
    };

    struct Ring {
        static constexpr uint32_t kSlots = 3;
        std::atomic<uint32_t> write_idx;
        std::atomic<uint32_t> read_idx;
        std::atomic<uint32_t> frame_count;
        Slot slots[kSlots];

        // SPSC ring for input events written by the native render thread and
        // read by the Wine capture thread.
        static constexpr uint32_t kInputSlots = 64;
        std::atomic<uint32_t> input_write{0};
        std::atomic<uint32_t> input_read{0};
        InputEvent input_slots[kInputSlots];
    };

    // Create new SHM owned by this process (native side).
    static std::unique_ptr<FrameSharedMemory> create(uint32_t max_w,
                                                      uint32_t max_h);
    // Open existing SHM created by another process (Wine side).
    static std::unique_ptr<FrameSharedMemory> open(const std::string& name);

    ~FrameSharedMemory() noexcept;

    FrameSharedMemory(const FrameSharedMemory&) = delete;
    FrameSharedMemory& operator=(const FrameSharedMemory&) = delete;

    const std::string& name() const noexcept { return name_; }
    bool valid() const noexcept { return data_ != nullptr; }

    // Wine (producer): call beginWrite/endWrite around pixel writes.
    uint8_t* beginWrite(uint32_t w, uint32_t h) noexcept;
    void endWrite() noexcept;

    // Native (consumer): call beginRead/endRead around pixel reads.
    const uint8_t* beginRead(uint32_t& w, uint32_t& h,
                              uint32_t& stride) noexcept;
    void endRead() noexcept;

    // Native render thread (producer): enqueue one input event.
    // Returns false if the ring is full (event dropped).
    bool writeInput(const InputEvent& ev) noexcept;
    // Wine capture thread (consumer): dequeue one input event.
    // Returns false if the ring is empty.
    bool readInput(InputEvent& ev) noexcept;

   private:
    FrameSharedMemory() = default;

    std::string name_;
    void* data_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
    bool owner_ = false;
    Ring* ring_ = nullptr;
    uint8_t* pixels_ = nullptr;
    size_t pixel_slot_size_ = 0;
    int current_slot_ = -1;
};

}  // namespace yabridge
