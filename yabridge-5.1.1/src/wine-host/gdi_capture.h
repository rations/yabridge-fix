// yabridge: a Wine plugin bridge - GDI window frame capture
// Captures a Win32 HWND's client area using BitBlt into a 32bpp BGRA buffer.
#pragma once

#include <cstdint>
#include <windows.h>

#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000
#endif

namespace yabridge {

class GdiCapture {
   public:
    GdiCapture() = default;
    ~GdiCapture() noexcept;

    GdiCapture(const GdiCapture&) = delete;
    GdiCapture& operator=(const GdiCapture&) = delete;

    // Set up GDI resources for hwnd. Re-call if the window is resized.
    bool initialize(HWND hwnd) noexcept;

    // Capture current frame into buf (must be width()*height()*4 bytes).
    // Returns false on failure; buf content is undefined on failure.
    bool capture(uint8_t* buf) noexcept;

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

   private:
    void cleanup() noexcept;

    HWND hwnd_ = nullptr;
    HDC hdc_window_ = nullptr;
    HDC hdc_mem_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace yabridge
