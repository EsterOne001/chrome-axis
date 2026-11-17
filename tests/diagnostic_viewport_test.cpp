#include "diagnostics_dialog.hpp"
#include "diagnostic_viewport.hpp"

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

bool containsVertically(const RECT& area, const RECT& control)
{
    return control.top >= area.top && control.bottom <= area.bottom;
}

bool contains(const RECT& area, const RECT& control)
{
    return containsVertically(area, control) && control.left >= area.left && control.right <= area.right;
}

RECT localBounds(HWND window, HWND control)
{
    auto rect = bounds(control);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

bool check(bool condition, const char* description)
{
    if (!condition)
        std::fprintf(stderr, "%s\n", description);
    return condition;
}

} // namespace

int main()
{
    const auto instance = GetModuleHandleW(nullptr);
    const auto window = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_DIAGNOSTICS), nullptr, dialogProc, 0);
    if (!check(window != nullptr, "create the diagnostic window"))
        return 1;
    const auto clip = GetDlgItem(window, IDC_DIAGNOSTICS_VIEWPORT);
    chrome_axis::DiagnosticViewport layout;
    layout.content = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_DIAGNOSTICS_CONTENT), clip, dialogProc, 0);
    if (!check(layout.content != nullptr, "create the diagnostic content")) {
        DestroyWindow(window);
        return 1;
    }

    const auto csvStatus = GetDlgItem(layout.content, IDC_DIAGNOSTICS_CSV_STATUS);
    const auto details = GetDlgItem(layout.content, IDC_DIAGNOSTICS_ACCESSORY_DETAILS);
    const auto horizontalBar = GetDlgItem(window, IDC_DIAGNOSTICS_HORIZONTAL_SCROLL);
    constexpr std::array buttons{IDC_DIAGNOSTICS_PERFORMANCE, IDC_DIAGNOSTICS_SAVE_CSV, IDC_DIAGNOSTICS_COPY, IDCANCEL};
    constexpr RECT full{0, 0, 1920, 1040};
    constexpr RECT compact{0, 20, 1366, 728};
    layout.fit(window, full);
    bool ok = check(!layout.canScroll() && !layout.canScrollHorizontally() && contains(bounds(clip), bounds(csvStatus)),
                    "full-size work area displays all diagnostic data");
    std::array<RECT, buttons.size()> fullFooter{};
    for (std::size_t i = 0; i < buttons.size(); ++i)
        fullFooter[i] = localBounds(window, GetDlgItem(window, buttons[i]));

    layout.fit(window, compact);
    ok &= check(layout.canScroll() && !layout.canScrollHorizontally() && contains(compact, bounds(window)),
                "compact work area activates scrolling and contains the window");
    for (const auto id : buttons)
        ok &= check(containsVertically(compact, bounds(GetDlgItem(window, id))), "footer action is inside the work area");
    const auto copy = GetDlgItem(window, IDC_DIAGNOSTICS_COPY);
    const auto copyPosition = bounds(copy);
    const auto viewportPosition = bounds(clip);
    layout.scroll(window, SB_BOTTOM);
    ok &= check(containsVertically(viewportPosition, bounds(csvStatus)), "scrolling exposes the final CSV status");
    const auto scrolledCopyPosition = bounds(copy);
    ok &= check(EqualRect(&copyPosition, &scrolledCopyPosition), "the action buttons remain fixed while scrolling");

    constexpr RECT small{0, 20, 1366, 392};
    layout.fit(window, small);
    layout.scroll(window, SB_BOTTOM);
    layout.showControl(window, details);
    ok &= check(containsVertically(bounds(clip), bounds(details)), "keyboard focus reveals the accessory details button");

    constexpr RECT narrow{80, 40, 763, 520};
    for (int cycle = 0; cycle < 3; ++cycle) {
        layout.fit(window, full);
        SetFocus(details);
        layout.fit(window, narrow);
        const auto viewport = bounds(clip);
        ok &= check(layout.canScroll() && layout.canScrollHorizontally() && contains(narrow, bounds(window))
                    && contains(narrow, bounds(horizontalBar)), "narrow work area contains the dialog and both scrollbars");
        ok &= check(GetFocus() == details && contains(viewport, bounds(details)),
                    "the focused Details action remains visible when resizing without another focus event");
        std::array<RECT, buttons.size()> footer{};
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            footer[i] = bounds(GetDlgItem(window, buttons[i]));
            ok &= check(contains(narrow, footer[i]) && footer[i].top >= bounds(horizontalBar).bottom,
                        "every action remains fully visible below both scrollbars");
        }
        ok &= check(footer[0].right < footer[1].left && footer[0].bottom < footer[2].top
                    && footer[2].right < footer[3].left, "narrow footer actions have room in two rows");
        layout.scroll(window, SB_BOTTOM);
        layout.scroll(window, SB_TOP, true);
        ok &= check(bounds(csvStatus).left >= viewport.left && containsVertically(viewport, bounds(csvStatus)),
                    "narrow view reveals the left end of the last line");
        layout.scroll(window, SB_BOTTOM, true);
        ok &= check(bounds(csvStatus).right <= viewport.right, "horizontal scrolling reveals the right end of the last line");
        layout.scroll(window, SB_TOP, true);
        layout.showControl(window, details);
        ok &= check(contains(viewport, bounds(details)), "keyboard focus reveals accessory details on both axes");
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            const auto afterScroll = bounds(GetDlgItem(window, buttons[i]));
            ok &= check(EqualRect(&footer[i], &afterScroll), "narrow footer actions stay fixed while scrolling");
        }

        layout.fit(window, full);
        ok &= check(!layout.canScroll() && !layout.canScrollHorizontally() && contains(bounds(clip), bounds(csvStatus)),
                    "returning to a large work area resets both scroll positions");
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            const auto rect = localBounds(window, GetDlgItem(window, buttons[i]));
            ok &= check(EqualRect(&fullFooter[i], &rect), "normal one-row action geometry is restored after narrowing");
        }
    }
    DestroyWindow(window);

    const auto accessoryWindow = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_DIAGNOSTICS_ACCESSORIES),
                                                     nullptr, dialogProc, 0);
    if (!check(accessoryWindow != nullptr, "create the accessory diagnostics window"))
        return 1;
    const auto accessoryClip = GetDlgItem(accessoryWindow, IDC_DIAGNOSTICS_VIEWPORT);
    const auto accessoryBar = GetDlgItem(accessoryWindow, IDC_DIAGNOSTICS_SCROLL);
    chrome_axis::DiagnosticViewport accessoryLayout;
    accessoryLayout.content = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_DIAGNOSTICS_ACCESSORIES_CONTENT),
                                                accessoryClip, dialogProc, 0);
    if (!check(accessoryLayout.content != nullptr, "create the body of the accessory diagnostics window")) {
        DestroyWindow(accessoryWindow);
        return 1;
    }
    const auto accessoryList = GetDlgItem(accessoryLayout.content, IDC_DIAGNOSTICS_COMMAND_LIST);
    const auto accessoryClose = GetDlgItem(accessoryWindow, IDCANCEL);
    const auto accessoryStatus = GetDlgItem(accessoryLayout.content, IDC_DIAGNOSTICS_COMMAND_SENT);
    SendMessageW(accessoryList, LB_SETITEMHEIGHT, 0, 24);
    SendMessageW(accessoryList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"First binding"));
    SendMessageW(accessoryList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Second binding"));
    SendMessageW(accessoryList, LB_SETCURSEL, 1, 0);
    constexpr RECT veryShort{80, 40, 763, 424};
    accessoryLayout.fitAccessory(accessoryWindow, full);
    const auto fullList = localBounds(accessoryWindow, accessoryList);
    const auto fullClose = localBounds(accessoryWindow, accessoryClose);
    for (int cycle = 0; cycle < 3; ++cycle) {
        accessoryLayout.fitAccessory(accessoryWindow, narrow);
        const auto list = bounds(accessoryList);
        const auto closure = bounds(accessoryClose);
        ok &= check(contains(narrow, bounds(accessoryWindow)) && contains(narrow, list) && contains(narrow, closure)
                    && !accessoryLayout.canScroll() && list.bottom - list.top >= 48 && contains(narrow, bounds(accessoryStatus))
                    && list.bottom < bounds(accessoryStatus).top && bounds(accessoryStatus).bottom < closure.top,
                    "accessory details keep the scrollable list, final reading and Close visible without overlap");
        ok &= check(SendMessageW(accessoryList, LB_GETCURSEL, 0, 0) == 1,
                    "changing accessory details geometry preserves the selected binding");

        SetFocus(accessoryList);
        accessoryLayout.fitAccessory(accessoryWindow, veryShort);
        const auto shortView = bounds(accessoryClip);
        const auto shortList = bounds(accessoryList);
        const auto shortClose = bounds(accessoryClose);
        ok &= check(accessoryLayout.canScroll() && contains(veryShort, bounds(accessoryBar))
                    && contains(veryShort, shortClose) && contains(shortView, shortList) && shortList.bottom - shortList.top >= 48
                    && GetFocus() == accessoryList, "very short accessory diagnostics keep two rows, focus and Close accessible");
        accessoryLayout.scroll(accessoryWindow, SB_BOTTOM);
        const auto scrolledClose = bounds(accessoryClose);
        ok &= check(contains(shortView, bounds(accessoryStatus)) && EqualRect(&shortClose, &scrolledClose),
                    "the accessory viewport can scroll to the final reading while Close stays fixed");
        ok &= check(SendMessageW(accessoryList, LB_GETCURSEL, 0, 0) == 1,
                    "scrolling the whole accessory panel preserves the selected binding");

        accessoryLayout.fitAccessory(accessoryWindow, full);
        const auto wideList = localBounds(accessoryWindow, accessoryList);
        const auto wideClose = localBounds(accessoryWindow, accessoryClose);
        ok &= check(EqualRect(&fullList, &wideList) && EqualRect(&fullClose, &wideClose),
                    "accessory diagnostics restore the native list and Close positions on a wide screen");
    }
    DestroyWindow(accessoryWindow);
    return ok ? 0 : 1;
}
