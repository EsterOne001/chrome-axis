#pragma once

#include <windows.h>

#include <vector>

namespace chrome_axis {

// Keep the original dialog children (and therefore their commands and tab order).
// When the monitor cannot fit the resource at its current DPI, pan that layout.
class DialogViewport {
public:
    void initialise(HWND window);
    void fit(HWND window);
    void fit(HWND window, const RECT& workArea);
    // For a resizable window whose caller lays out its children using extent() and offset().
    void resizeContent(HWND window, SIZE minimum);
    bool scroll(HWND window, int action, bool horizontal);
    bool wheel(HWND window, int delta, bool horizontal);
    void reveal(HWND window, HWND control);
    POINT offset() const noexcept { return offset_; }
    SIZE extent() const noexcept { return content_; }
    int layoutWidth() const noexcept { return content_.cx; }

private:
    struct Child {
        HWND window;
        POINT origin;
    };

    void scrollTo(HWND window, POINT position, bool reposition = false);

    std::vector<Child> children_;
    SIZE design_{};
    SIZE content_{};
    SIZE page_{};
    POINT offset_{};
    UINT designDpi_{USER_DEFAULT_SCREEN_DPI};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    int horizontalWheelRemainder_{};
    int verticalWheelRemainder_{};
};

} // namespace chrome_axis
