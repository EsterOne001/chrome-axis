#include "dialog_viewport.hpp"

#include <algorithm>

namespace chrome_axis {
namespace {

RECT clientBounds(HWND window)
{
    RECT bounds{};
    GetClientRect(window, &bounds);
    return bounds;
}

void updateScrollBar(HWND window, int bar, LONG extent, LONG page, LONG position)
{
    if (page >= extent) {
        ShowScrollBar(window, bar, FALSE);
        return;
    }
    SCROLLINFO info{sizeof(SCROLLINFO), SIF_PAGE | SIF_POS | SIF_RANGE, 0, extent - 1,
                    static_cast<UINT>(page), position, 0};
    SetScrollInfo(window, bar, &info, TRUE);
    ShowScrollBar(window, bar, TRUE);
}

} // namespace

void DialogViewport::initialise(HWND window)
{
    designDpi_ = GetDpiForWindow(window);
    if (designDpi_ == 0)
        designDpi_ = USER_DEFAULT_SCREEN_DPI;
    const auto bounds = clientBounds(window);
    design_ = {bounds.right, bounds.bottom};
    for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect{};
        if (GetWindowRect(child, &rect)) {
            POINT origin{rect.left, rect.top};
            MapWindowPoints(HWND_DESKTOP, window, &origin, 1);
            children_.push_back({child, origin});
        }
    }
}

void DialogViewport::fit(HWND window)
{
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        fit(window, monitor.rcWork);
}

void DialogViewport::fit(HWND window, const RECT& workArea)
{
    const auto width = workArea.right - workArea.left;
    const auto height = workArea.bottom - workArea.top;
    if (width <= 0 || height <= 0 || design_.cx <= 0 || design_.cy <= 0)
        return;

    RECT bounds{};
    GetWindowRect(window, &bounds);
    const auto previousDpi = dpi_;
    if (const auto dpi = GetDpiForWindow(window))
        dpi_ = dpi;
    const auto scale = [this](LONG value) { return MulDiv(value, dpi_, designDpi_); };
    content_ = {scale(design_.cx), scale(design_.cy)};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, style & ~(WS_HSCROLL | WS_VSCROLL), FALSE, exStyle, dpi_);
    const auto outerWidth = content_.cx + frame.right - frame.left;
    const auto outerHeight = content_.cy + frame.bottom - frame.top;
    const auto barWidth = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
    const auto barHeight = GetSystemMetricsForDpi(SM_CYHSCROLL, dpi_);
    const bool horizontal = outerWidth + (outerHeight > height ? barWidth : 0) > width;
    const bool vertical = outerHeight + (horizontal ? barHeight : 0) > height;
    const auto fittedWidth = std::min(width, outerWidth + (vertical ? barWidth : 0));
    const auto fittedHeight = std::min(height, outerHeight + (horizontal ? barHeight : 0));
    const auto left = std::clamp(bounds.left, workArea.left, workArea.right - fittedWidth);
    const auto top = std::clamp(bounds.top, workArea.top, workArea.bottom - fittedHeight);

    // Show or hide native scrollbars before measuring the true client area.
    ShowScrollBar(window, SB_HORZ, horizontal);
    ShowScrollBar(window, SB_VERT, vertical);
    if (bounds.left != left || bounds.top != top || bounds.right != left + fittedWidth ||
        bounds.bottom != top + fittedHeight)
        SetWindowPos(window, nullptr, left, top, fittedWidth, fittedHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    const auto client = clientBounds(window);
    page_ = {client.right, client.bottom};
    scrollTo(window, {MulDiv(offset_.x, dpi_, previousDpi), MulDiv(offset_.y, dpi_, previousDpi)}, true);
    reveal(window, GetFocus());
}

void DialogViewport::resizeContent(HWND window, SIZE minimum)
{
    const auto previousDpi = dpi_;
    if (const auto dpi = GetDpiForWindow(window))
        dpi_ = dpi;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const bool hadVertical = (style & WS_VSCROLL) != 0;
    const bool hadHorizontal = (style & WS_HSCROLL) != 0;
    const auto barWidth = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
    const auto barHeight = GetSystemMetricsForDpi(SM_CYHSCROLL, dpi_);
    const auto old = clientBounds(window);
    const auto width = old.right + (hadVertical ? barWidth : 0);
    const auto height = old.bottom + (hadHorizontal ? barHeight : 0);
    const bool horizontal = width < minimum.cx + (height < minimum.cy ? barWidth : 0);
    const bool vertical = height < minimum.cy + (horizontal ? barHeight : 0);
    if (hadHorizontal != horizontal)
        ShowScrollBar(window, SB_HORZ, horizontal);
    if (hadVertical != vertical)
        ShowScrollBar(window, SB_VERT, vertical);
    const auto client = clientBounds(window);
    page_ = {client.right, client.bottom};
    content_ = {std::max(minimum.cx, page_.cx), std::max(minimum.cy, page_.cy)};
    scrollTo(window, {MulDiv(offset_.x, dpi_, previousDpi), MulDiv(offset_.y, dpi_, previousDpi)}, true);
}

void DialogViewport::scrollTo(HWND window, POINT position, bool reposition)
{
    const POINT next{std::clamp(position.x, 0L, std::max(0L, content_.cx - page_.cx)),
                     std::clamp(position.y, 0L, std::max(0L, content_.cy - page_.cy))};
    if (!reposition && next.x == offset_.x && next.y == offset_.y)
        return;
    offset_ = next;
    updateScrollBar(window, SB_HORZ, content_.cx, page_.cx, next.x);
    updateScrollBar(window, SB_VERT, content_.cy, page_.cy, next.y);
    for (const auto& child : children_) {
        SetWindowPos(child.window, nullptr, MulDiv(child.origin.x, dpi_, designDpi_) - next.x,
                     MulDiv(child.origin.y, dpi_, designDpi_) - next.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool DialogViewport::scroll(HWND window, int action, bool horizontal)
{
    const int range = horizontal ? content_.cx - page_.cx : content_.cy - page_.cy;
    if (range <= 0)
        return false;
    auto point = offset_;
    auto& position = horizontal ? point.x : point.y;
    const int line = MulDiv(16, dpi_, USER_DEFAULT_SCREEN_DPI);
    const auto page = horizontal ? page_.cx : page_.cy;
    switch (action) {
    case SB_LINEUP: position -= line; break;
    case SB_LINEDOWN: position += line; break;
    case SB_PAGEUP: position -= page; break;
    case SB_PAGEDOWN: position += page; break;
    case SB_TOP: position = 0; break;
    case SB_BOTTOM: position = range; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_TRACKPOS};
        if (GetScrollInfo(window, horizontal ? SB_HORZ : SB_VERT, &info))
            position = info.nTrackPos;
        break;
    }
    }
    scrollTo(window, point);
    return true;
}

bool DialogViewport::wheel(HWND window, int delta, bool horizontal)
{
    const int range = horizontal ? content_.cx - page_.cx : content_.cy - page_.cy;
    if (range <= 0)
        return false;
    UINT lines = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    auto& remainder = horizontal ? horizontalWheelRemainder_ : verticalWheelRemainder_;
    remainder += delta;
    const auto page = horizontal ? page_.cx : page_.cy;
    const auto step = lines == WHEEL_PAGESCROLL ? page : static_cast<LONG>(lines) * MulDiv(16, dpi_, USER_DEFAULT_SCREEN_DPI);
    auto point = offset_;
    (horizontal ? point.x : point.y) += (horizontal ? 1 : -1) * (remainder / WHEEL_DELTA) * step;
    scrollTo(window, point);
    remainder %= WHEEL_DELTA;
    return true;
}

void DialogViewport::reveal(HWND window, HWND control)
{
    if (!control || !IsChild(window, control) || !(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE))
        return;
    RECT rect{};
    GetWindowRect(control, &rect);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rect), 2);
    const auto revealAxis = [](LONG start, LONG end, LONG length, LONG next) {
        if (start < 0 || end - start > length)
            return next + start;
        return end > length ? next + end - length : next;
    };
    scrollTo(window, {revealAxis(rect.left, rect.right, page_.cx, offset_.x),
                      revealAxis(rect.top, rect.bottom, page_.cy, offset_.y)});
}

} // namespace chrome_axis
