#pragma once

#include <windows.h>

namespace chrome_axis {

class DiagnosticViewport {
public:
    HWND content{};

    void fit(HWND window, const RECT& workArea);
    void scroll(HWND window, int action);
    void wheel(HWND window, int delta);
    void showControl(HWND window, HWND control);
    bool canScroll() const noexcept { return visibleHeight < contentHeight; }

private:
    void scrollTo(HWND window, int position);

    int contentHeight{};
    int visibleHeight{};
    int offset{};
    int lineHeight{};
    int wheelRemainder{};
};

} // namespace chrome_axis
