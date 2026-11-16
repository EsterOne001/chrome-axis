#pragma once

#include <windows.h>

namespace chrome_axis {

// DirectInput retains its cooperative HWND. The same worker owns the window
// and destroys it only after releasing its devices.
class InputWindow {
public:
    InputWindow()
        : window_(CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
              0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)) {}

    ~InputWindow() { if (window_) DestroyWindow(window_); }
    InputWindow(const InputWindow&) = delete;
    InputWindow& operator=(const InputWindow&) = delete;

    HWND get() const noexcept { return window_; }

private:
    HWND window_{};
};

} // namespace chrome_axis
