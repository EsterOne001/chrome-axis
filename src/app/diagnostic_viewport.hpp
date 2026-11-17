#pragma once

#include <windows.h>

namespace chrome_axis {

class DiagnosticViewport {
public:
    HWND content{};

    void fit(HWND window, const RECT& workArea);
    void fitAccessory(HWND window, const RECT& workArea);
    void scroll(HWND window, int action, bool horizontal = false);
    void wheel(HWND window, int delta, bool horizontal = false);
    void showControl(HWND window, HWND control);
    bool canScroll() const noexcept { return visibleHeight < contentHeight; }
    bool canScrollHorizontally() const noexcept { return visibleWidth < contentWidth; }

private:
    void scrollTo(HWND window, int x, int y);
    void revealFocus(HWND window);

    int contentWidth{};
    int visibleWidth{};
    int horizontalOffset{};
    int contentHeight{};
    int visibleHeight{};
    int verticalOffset{};
    int lineHeight{};
    int horizontalWheelRemainder{};
    int verticalWheelRemainder{};
};

} // namespace chrome_axis
