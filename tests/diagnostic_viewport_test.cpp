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
    constexpr RECT full{0, 0, 1920, 1040};
    constexpr RECT compact{0, 20, 1366, 728};
    layout.fit(window, full);
    bool ok = check(!layout.canScroll() && containsVertically(bounds(clip), bounds(csvStatus)),
                    "full-size work area displays all diagnostic data");

    layout.fit(window, compact);
    ok &= check(layout.canScroll() && containsVertically(compact, bounds(window)),
                "compact work area activates scrolling and contains the window");
    constexpr std::array buttons{IDC_DIAGNOSTICS_PERFORMANCE, IDC_DIAGNOSTICS_SAVE_CSV, IDC_DIAGNOSTICS_COPY, IDCANCEL};
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

    layout.fit(window, full);
    ok &= check(!layout.canScroll() && containsVertically(bounds(clip), bounds(csvStatus)),
                "returning to a large work area resets the scroll position");
    DestroyWindow(window);
    return ok ? 0 : 1;
}
