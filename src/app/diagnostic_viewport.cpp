#include "diagnostic_viewport.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "diagnostics_dialog.hpp"

#include <algorithm>
#include <array>

namespace chrome_axis {
namespace {

struct FooterAction {
    int id;
    LONG left;
    LONG width;
};

// Normal positions from IDD_DIAGNOSTICS. Keep them in dialog units for DPI changes and for returning from narrow mode.
constexpr std::array footerActions{
    FooterAction{IDC_DIAGNOSTICS_PERFORMANCE, 24, 116},
    FooterAction{IDC_DIAGNOSTICS_SAVE_CSV, 144, 77},
    FooterAction{IDC_DIAGNOSTICS_COPY, 225, 88},
    FooterAction{IDCANCEL, 317, 83}
};

struct FittedDialog {
    int clientWidth;
    int removedWidth;
    int removedHeight;
};

FittedDialog fitDialog(HWND window, const RECT& workArea, RECT design)
{
    RECT windowBounds{}, client{};
    GetWindowRect(window, &windowBounds);
    GetClientRect(window, &client);
    MapDialogRect(window, &design);
    const LONG frameHeight = windowBounds.bottom - windowBounds.top - client.bottom;
    const LONG frameWidth = windowBounds.right - windowBounds.left - client.right;
    const LONG height = std::min(design.bottom + frameHeight, workArea.bottom - workArea.top);
    const LONG width = std::min(design.right + frameWidth, workArea.right - workArea.left);
    const LONG x = std::clamp(windowBounds.left, workArea.left, workArea.right - width);
    const LONG y = std::clamp(windowBounds.top, workArea.top, workArea.bottom - height);
    if (windowBounds.left != x || windowBounds.top != y ||
        windowBounds.right - x != width || windowBounds.bottom - y != height)
        SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    return {static_cast<int>(std::max<LONG>(0, width - frameWidth)), static_cast<int>(design.right + frameWidth - width),
            static_cast<int>(design.bottom + frameHeight - height)};
}

void setScrollRange(HWND bar, int extent, int page, int position)
{
    SCROLLINFO scrollbar{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS};
    scrollbar.nMax = std::max(0, extent - 1);
    scrollbar.nPage = static_cast<UINT>(page);
    scrollbar.nPos = position;
    SetScrollInfo(bar, SB_CTL, &scrollbar, TRUE);
}

void placeFooter(HWND window, int y, bool narrow, int clientWidth)
{
    RECT margin{0, 0, 24, 0};
    RECT gap{0, 0, 4, 4};
    MapDialogRect(window, &margin);
    MapDialogRect(window, &gap);
    const LONG edge = std::clamp<LONG>(margin.right, 0, clientWidth / 8);
    const LONG columnsWidth = std::max<LONG>(0, clientWidth - 2 * edge - gap.right);
    for (std::size_t index = 0; index < footerActions.size(); ++index) {
        const auto& action = footerActions[index];
        RECT rect{action.left, 0, action.width, 20};
        MapDialogRect(window, &rect);
        if (narrow) {
            const LONG split = columnsWidth / 2;
            rect.left = index % 2 == 0 ? edge : edge + split + gap.right;
            rect.right = index % 2 == 0 ? split : columnsWidth - split;
            rect.top = index < 2 ? 0 : rect.bottom + gap.bottom;
        }
        SetWindowPos(GetDlgItem(window, action.id), nullptr, rect.left, y + rect.top, rect.right, rect.bottom,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

} // namespace

void DiagnosticViewport::scrollTo(HWND window, int x, int y)
{
    horizontalOffset = std::clamp(x, 0, std::max(0, contentWidth - visibleWidth));
    verticalOffset = std::clamp(y, 0, std::max(0, contentHeight - visibleHeight));
    setScrollRange(GetDlgItem(window, IDC_DIAGNOSTICS_HORIZONTAL_SCROLL), contentWidth, visibleWidth, horizontalOffset);
    setScrollRange(GetDlgItem(window, IDC_DIAGNOSTICS_SCROLL), contentHeight, visibleHeight, verticalOffset);
    SetWindowPos(content, nullptr, -horizontalOffset, -verticalOffset, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void DiagnosticViewport::fit(HWND window, const RECT& workArea)
{
    RECT body{414, 54, 424, 422};
    RECT footer{0, 426, 0, 437};
    RECT extraFooterRow{0, 0, 0, 24};
    MapDialogRect(window, &body);
    MapDialogRect(window, &footer);
    MapDialogRect(window, &extraFooterRow);
    const int previousLineHeight = lineHeight;
    const int previousContentWidth = contentWidth;
    lineHeight = footer.bottom - footer.top;
    const auto [clientWidth, removedWidth, removedHeight] = fitDialog(window, workArea, {0, 0, 424, 453});
    contentWidth = body.right;
    contentHeight = body.bottom - body.top;
    const bool narrow = removedWidth > 0;
    const int lowerBy = removedHeight + (narrow ? extraFooterRow.bottom : 0);
    const int availableBodyHeight = std::max(0, contentHeight - lowerBy);
    const int horizontalBarHeight = narrow
        ? std::min(availableBodyHeight, GetSystemMetricsForDpi(SM_CYHSCROLL, GetDpiForWindow(window))) : 0;
    visibleHeight = availableBodyHeight - horizontalBarHeight;
    const bool vertScroll = canScroll();
    const int verticalBarWidth = body.right - body.left;
    visibleWidth = narrow && vertScroll ? std::max(0, clientWidth - verticalBarWidth) : clientWidth;

    SetWindowPos(GetDlgItem(window, IDC_DIAGNOSTICS_VIEWPORT), HWND_BOTTOM, 0, body.top,
                 visibleWidth, visibleHeight, SWP_NOACTIVATE);
    SetWindowPos(content, nullptr, -horizontalOffset, -verticalOffset, contentWidth, contentHeight,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    const auto verticalBar = GetDlgItem(window, IDC_DIAGNOSTICS_SCROLL);
    const auto horizontalBar = GetDlgItem(window, IDC_DIAGNOSTICS_HORIZONTAL_SCROLL);
    const auto focus = GetFocus();
    if ((!vertScroll && focus == verticalBar) || (!narrow && focus == horizontalBar))
        SetFocus(GetDlgItem(window, IDCANCEL));
    SetWindowPos(verticalBar, HWND_TOP, std::max(0, clientWidth - verticalBarWidth), body.top, verticalBarWidth,
                 visibleHeight, SWP_NOACTIVATE | (vertScroll ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    SetWindowPos(horizontalBar, HWND_TOP, 0, body.top + visibleHeight, visibleWidth, horizontalBarHeight,
                 SWP_NOACTIVATE | (narrow ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    placeFooter(window, footer.top - lowerBy, narrow, clientWidth);
    scrollTo(window, previousContentWidth > 0 ? MulDiv(horizontalOffset, contentWidth, previousContentWidth) : 0,
             previousLineHeight > 0 ? MulDiv(verticalOffset, lineHeight, previousLineHeight) : 0);
}

void fitAccessoryDiagnosticsViewport(HWND window, const RECT& workArea)
{
    const auto fitted = fitDialog(window, workArea, {0, 0, 424, 267});
    const auto place = [&](int id, RECT geometry) {
        MapDialogRect(window, &geometry);
        if (id == IDCANCEL) {
            geometry.left -= fitted.removedWidth;
            geometry.top -= fitted.removedHeight;
        } else {
            geometry.right = std::max<LONG>(0, geometry.right - fitted.removedWidth);
            if (id == IDC_MODAL_RESULTS || id == IDC_DIAGNOSTICS_COMMAND_LIST)
                geometry.bottom = std::max<LONG>(0, geometry.bottom - fitted.removedHeight);
            else if (id == IDC_DIAGNOSTICS_COMMAND_INPUT || id == IDC_DIAGNOSTICS_COMMAND_OUTPUT
                     || id == IDC_DIAGNOSTICS_COMMAND_SENT)
                geometry.top -= fitted.removedHeight;
        }
        SetWindowPos(GetDlgItem(window, id), nullptr, std::max<LONG>(0, geometry.left), std::max<LONG>(0, geometry.top),
                     std::min<LONG>(geometry.right, fitted.clientWidth), geometry.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
    };
    // On a short screen give up list height first so the selected binding and Close stay accessible.
    place(IDC_MODAL_FORM, {12, 56, 400, 36});
    place(IDC_MODAL_RESULTS, {12, 98, 400, 135});
    place(IDC_DIAGNOSTICS_COMMAND_CONTEXT, {24, 63, 376, 21});
    place(IDC_MODAL_LIST_HEADING, {24, 103, 376, 14});
    place(IDC_DIAGNOSTICS_COMMAND_LIST, {24, 120, 376, 68});
    place(IDC_DIAGNOSTICS_COMMAND_INPUT, {24, 192, 376, 11});
    place(IDC_DIAGNOSTICS_COMMAND_OUTPUT, {24, 205, 376, 11});
    place(IDC_DIAGNOSTICS_COMMAND_SENT, {24, 218, 376, 11});
    place(IDCANCEL, {317, 240, 83, 20});
}

void DiagnosticViewport::scroll(HWND window, int action, bool horizontal)
{
    const int page = horizontal ? visibleWidth : visibleHeight;
    const int extent = horizontal ? contentWidth : contentHeight;
    int position = horizontal ? horizontalOffset : verticalOffset;
    switch (action) {
    case SB_LINEUP: position -= lineHeight; break;
    case SB_LINEDOWN: position += lineHeight; break;
    case SB_PAGEUP: position -= page; break;
    case SB_PAGEDOWN: position += page; break;
    case SB_TOP: position = 0; break;
    case SB_BOTTOM: position = extent; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO scrollbar{sizeof(SCROLLINFO), SIF_TRACKPOS};
        const auto id = horizontal ? IDC_DIAGNOSTICS_HORIZONTAL_SCROLL : IDC_DIAGNOSTICS_SCROLL;
        if (!GetScrollInfo(GetDlgItem(window, id), SB_CTL, &scrollbar))
            return;
        position = scrollbar.nTrackPos;
        break;
    }
    default: return;
    }
    scrollTo(window, horizontal ? position : horizontalOffset, horizontal ? verticalOffset : position);
}

void DiagnosticViewport::wheel(HWND window, int delta, bool horizontal)
{
    UINT lines = 3;
    SystemParametersInfoW(horizontal ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    auto& wheelRemainder = horizontal ? horizontalWheelRemainder : verticalWheelRemainder;
    wheelRemainder += delta;
    const auto page = horizontal ? visibleWidth : visibleHeight;
    const int step = lines == WHEEL_PAGESCROLL ? page : static_cast<int>(lines) * lineHeight;
    const auto movement = wheelRemainder / WHEEL_DELTA * step;
    scrollTo(window, horizontal ? horizontalOffset + movement : horizontalOffset,
             horizontal ? verticalOffset : verticalOffset - movement);
    wheelRemainder %= WHEEL_DELTA;
}

void DiagnosticViewport::showControl(HWND window, HWND control)
{
    RECT bounds{};
    GetWindowRect(control, &bounds);
    MapWindowPoints(HWND_DESKTOP, content, reinterpret_cast<POINT*>(&bounds), 2);
    const auto reveal = [](int first, int last, int offset, int page) {
        if (first < offset)
            return first;
        return last > offset + page ? last - page : offset;
    };
    scrollTo(window, reveal(bounds.left, bounds.right, horizontalOffset, visibleWidth),
             reveal(bounds.top, bounds.bottom, verticalOffset, visibleHeight));
}

} // namespace chrome_axis
