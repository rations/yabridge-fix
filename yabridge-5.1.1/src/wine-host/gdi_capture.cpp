// yabridge: a Wine plugin bridge - GDI frame capture implementation.
#include "gdi_capture.h"

#include <cstring>

namespace yabridge {

GdiCapture::~GdiCapture() noexcept {
    cleanup();
}

void GdiCapture::cleanup() noexcept {
    if (bitmap_) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    if (hdc_mem_) {
        DeleteDC(hdc_mem_);
        hdc_mem_ = nullptr;
    }
    if (hdc_window_ && hwnd_) {
        ReleaseDC(hwnd_, hdc_window_);
        hdc_window_ = nullptr;
    }
    hwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool GdiCapture::initialize(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    cleanup();

    hwnd_ = hwnd;

    RECT rect{};
    if (!GetClientRect(hwnd_, &rect)) return false;

    width_ = rect.right - rect.left;
    height_ = rect.bottom - rect.top;
    if (width_ <= 0 || height_ <= 0) return false;

    hdc_window_ = GetDC(hwnd_);
    if (!hdc_window_) return false;

    hdc_mem_ = CreateCompatibleDC(hdc_window_);
    if (!hdc_mem_) {
        cleanup();
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width_;
    bmi.bmiHeader.biHeight = -height_;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    bitmap_ =
        CreateDIBSection(hdc_mem_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_) {
        cleanup();
        return false;
    }

    SelectObject(hdc_mem_, bitmap_);
    return true;
}

bool GdiCapture::capture(uint8_t* buf) noexcept {
    if (!hwnd_ || !hdc_window_ || !hdc_mem_ || !buf) return false;

    // Reinitialize if window was resized
    RECT rect{};
    if (!GetClientRect(hwnd_, &rect)) return false;
    const int new_w = rect.right - rect.left;
    const int new_h = rect.bottom - rect.top;
    if (new_w != width_ || new_h != height_) {
        if (!initialize(hwnd_)) return false;
    }

    if (!BitBlt(hdc_mem_, 0, 0, width_, height_, hdc_window_, 0, 0,
                SRCCOPY | CAPTUREBLT))
        return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width_;
    bmi.bmiHeader.biHeight = -height_;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const int lines = GetDIBits(hdc_mem_, bitmap_, 0, (UINT)height_, buf,
                                &bmi, DIB_RGB_COLORS);
    if (lines != height_) return false;
    // BI_RGB leaves alpha=0; set to 0xFF so X11 depth-32 ARGB compositors
    // treat pixels as opaque rather than transparent.
    for (int i = 3; i < width_ * height_ * 4; i += 4)
        buf[i] = 0xFF;
    return true;
}

}  // namespace yabridge
