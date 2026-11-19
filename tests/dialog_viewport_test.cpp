#include "dialog_viewport.hpp"
#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/brake_dialog.hpp"
#include "chrome_axis/feedback_dialog.hpp"

#include <commctrl.h>

#include <array>
#include <cstdio>

namespace {

INT_PTR CALLBACK dialogProc(HWND, UINT message, WPARAM, LPARAM)
{
    return message == WM_INITDIALOG;
}

RECT bounds(HWND window)
{
    RECT rect{};
    GetWindowRect(window, &rect);
    return rect;
}

RECT clientBounds(HWND window)
{
    RECT rect{};
    GetClientRect(window, &rect);
    MapWindowPoints(window, HWND_DESKTOP, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

RECT localBounds(HWND window, HWND control)
{
    auto rect = bounds(control);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

bool contains(const RECT& area, const RECT& rect)
{
    return rect.left >= area.left && rect.right <= area.right &&
           rect.top >= area.top && rect.bottom <= area.bottom;
}

bool check(bool condition, int dialogId, const char* message)
{
    if (!condition)
        std::fprintf(stderr, "Dialog %d: %s\n", dialogId, message);
    return condition;
}

bool verify(int dialogId)
{
    const auto window = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(dialogId),
                                            nullptr, dialogProc, 0);
    if (!check(window != nullptr, dialogId, "create the real resource dialog"))
        return false;

    const auto accept = GetDlgItem(window, IDOK);
    const auto cancel = GetDlgItem(window, IDCANCEL);
    const auto original = localBounds(window, cancel);
    const auto originalSize = bounds(window);
    const auto width = originalSize.right - originalSize.left;
    const auto height = originalSize.bottom - originalSize.top;
    const RECT full{0, 0, width + 100, height + 100};
    const RECT shortArea{11, 23, 11 + width + 100, 23 + height * 4 / 5};
    const RECT narrow{13, 19, 13 + width * 3 / 4, 19 + height * 3 / 4};
    std::printf("Dialog %d at %u DPI: %ld x %ld px\n", dialogId, GetDpiForWindow(window), width, height);
    chrome_axis::DialogViewport layout;
    layout.initialise(window);
    layout.fit(window, full);
    auto restored = localBounds(window, cancel);
    auto restoredSize = bounds(window);
    bool ok = check(EqualRect(&original, &restored) &&
                    restoredSize.right - restoredSize.left == width && restoredSize.bottom - restoredSize.top == height,
                    dialogId, "a full-size work area preserves the resource geometry");

    for (int cycle = 0; cycle < 3; ++cycle) {
        layout.fit(window, shortArea);
        ok &= check(contains(shortArea, bounds(window)) && !contains(clientBounds(window), bounds(cancel)),
                    dialogId, "a short work area contains the window and clips the initial footer");
        ok &= check(layout.scroll(window, SB_BOTTOM, false) && contains(clientBounds(window), bounds(cancel)),
                    dialogId, "vertical scrollbar reaches Cancel");
        layout.scroll(window, SB_TOP, false);
        layout.reveal(window, accept);
        ok &= check(contains(clientBounds(window), bounds(accept)) &&
                    GetNextDlgTabItem(window, accept, FALSE) == cancel,
                    dialogId, "keyboard focus reveals Apply and the original next tab stop is Cancel");
        layout.reveal(window, cancel);
        ok &= check(contains(clientBounds(window), bounds(cancel)),
                    dialogId, "keyboard focus also reveals Cancel");

        layout.fit(window, narrow);
        layout.scroll(window, SB_TOP, false);
        layout.scroll(window, SB_TOP, true);
        ok &= check(contains(narrow, bounds(window)) && !contains(clientBounds(window), bounds(cancel)),
                    dialogId, "narrow work area shows the beginning of the layout");
        layout.reveal(window, cancel);
        ok &= check(layout.offset().x > 0 && layout.offset().y > 0 &&
                    contains(clientBounds(window), bounds(cancel)), dialogId,
                    "focus can reveal the last button on both axes");

        layout.fit(window, full);
        restored = localBounds(window, cancel);
        restoredSize = bounds(window);
        ok &= check(layout.offset().x == 0 && layout.offset().y == 0 &&
                    EqualRect(&original, &restored) &&
                    restoredSize.right - restoredSize.left == width && restoredSize.bottom - restoredSize.top == height &&
                    !(GetWindowLongPtrW(window, GWL_STYLE) & (WS_HSCROLL | WS_VSCROLL)),
                    dialogId, "large work area restores the original geometry after a scroll cycle");
    }
    DestroyWindow(window);
    return ok;
}

} // namespace

int main()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    constexpr std::array dialogs{IDD_ACCESSORIES, IDD_SHIFTER, IDD_BRAKE_SETTINGS, IDD_PEDAL_SETTINGS,
                                 IDD_FORCE_FEEDBACK};
    bool ok = true;
    for (const auto dialog : dialogs)
        ok &= verify(dialog);
    return ok ? 0 : 1;
}
