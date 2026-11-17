#include "diagnostic_viewport.hpp"

#include "diagnostics_dialog.hpp"

#include <algorithm>

namespace chrome_axis {

void DiagnosticViewport::scrollTo(HWND window, int position)
{
    offset = std::clamp(position, 0, std::max(0, contentHeight - visibleHeight));
    SCROLLINFO scrollbar{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
    scrollbar.nMax = std::max(0, contentHeight - 1);
    scrollbar.nPage = static_cast<UINT>(visibleHeight);
    scrollbar.nPos = offset;
    SetScrollInfo(GetDlgItem(window, IDC_DIAGNOSTICS_SCROLL), SB_CTL, &scrollbar, TRUE);
    SetWindowPos(content, nullptr, 0, -offset, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void DiagnosticViewport::fit(HWND window, const RECT& workArea)
{
    RECT windowBounds{}, client{};
    GetWindowRect(window, &windowBounds);
    GetClientRect(window, &client);

    RECT design{0, 0, 424, 453};
    RECT body{414, 54, 424, 422};
    RECT footer{0, 426, 0, 437};
    MapDialogRect(window, &design);
    MapDialogRect(window, &body);
    MapDialogRect(window, &footer);
    const int previousLineHeight = lineHeight;
    lineHeight = footer.bottom - footer.top;
    const LONG frameHeight = windowBounds.bottom - windowBounds.top - client.bottom;
    const LONG height = std::min(design.bottom + frameHeight, workArea.bottom - workArea.top);
    const LONG width = design.right + windowBounds.right - windowBounds.left - client.right;
    const LONG x = std::clamp(windowBounds.left, workArea.left,
                              std::max(workArea.left, workArea.right - width));
    const LONG y = std::clamp(windowBounds.top, workArea.top, workArea.bottom - height);
    if (windowBounds.left != x || windowBounds.top != y ||
        windowBounds.right - x != width || windowBounds.bottom - y != height)
        SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    const int shorterBy = design.bottom + frameHeight - height;
    contentHeight = body.bottom - body.top;
    visibleHeight = std::max(0, contentHeight - shorterBy);
    SetWindowPos(GetDlgItem(window, IDC_DIAGNOSTICS_VIEWPORT), HWND_BOTTOM, 0, body.top,
                 body.right, visibleHeight, SWP_NOACTIVATE);
    SetWindowPos(content, nullptr, 0, -offset, body.right, contentHeight,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    const auto scroll = GetDlgItem(window, IDC_DIAGNOSTICS_SCROLL);
    const bool canScroll = visibleHeight < contentHeight;
    if (!canScroll && GetFocus() == scroll)
        SetFocus(GetDlgItem(window, IDCANCEL));
    SetWindowPos(scroll, HWND_TOP, body.left, body.top, body.right - body.left, visibleHeight,
                 SWP_NOACTIVATE | (canScroll ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    for (const auto buttonId : {IDC_DIAGNOSTICS_PERFORMANCE, IDC_DIAGNOSTICS_SAVE_CSV, IDC_DIAGNOSTICS_COPY, IDCANCEL}) {
        const auto button = GetDlgItem(window, buttonId);
        RECT bounds{};
        GetWindowRect(button, &bounds);
        POINT left{bounds.left, bounds.top};
        ScreenToClient(window, &left);
        SetWindowPos(button, nullptr, left.x, footer.top - shorterBy, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }
    scrollTo(window, previousLineHeight > 0 ? MulDiv(offset, lineHeight, previousLineHeight) : 0);
}

void DiagnosticViewport::scroll(HWND window, int action)
{
    switch (action) {
    case SB_LINEUP: scrollTo(window, offset - lineHeight); break;
    case SB_LINEDOWN: scrollTo(window, offset + lineHeight); break;
    case SB_PAGEUP: scrollTo(window, offset - visibleHeight); break;
    case SB_PAGEDOWN: scrollTo(window, offset + visibleHeight); break;
    case SB_TOP: scrollTo(window, 0); break;
    case SB_BOTTOM: scrollTo(window, contentHeight); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO scrollbar{sizeof(SCROLLINFO), SIF_TRACKPOS};
        if (GetScrollInfo(GetDlgItem(window, IDC_DIAGNOSTICS_SCROLL), SB_CTL, &scrollbar))
            scrollTo(window, scrollbar.nTrackPos);
        break;
    }
    }
}

void DiagnosticViewport::wheel(HWND window, int delta)
{
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    wheelRemainder += delta;
    const int step = lines == WHEEL_PAGESCROLL ? visibleHeight : static_cast<int>(lines) * lineHeight;
    scrollTo(window, offset - wheelRemainder / WHEEL_DELTA * step);
    wheelRemainder %= WHEEL_DELTA;
}

void DiagnosticViewport::showControl(HWND window, HWND control)
{
    RECT bounds{};
    GetWindowRect(control, &bounds);
    MapWindowPoints(HWND_DESKTOP, content, reinterpret_cast<POINT*>(&bounds), 2);
    if (bounds.top < offset)
        scrollTo(window, bounds.top);
    else if (bounds.bottom > offset + visibleHeight)
        scrollTo(window, bounds.bottom - visibleHeight);
}

} // namespace chrome_axis
