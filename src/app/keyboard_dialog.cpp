#include "chrome_axis/keyboard_dialog.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"
#include "ui_theme.hpp"

#include <array>
#include <bitset>
#include <string>
#include <string_view>

namespace chrome_axis {
namespace {

constexpr int inactiveKeyId = 2800;
constexpr int firstKeyId = 2900;
constexpr int keyPitch = 17;
constexpr int keyWidth = 16;
constexpr int keyHeight = 16;

struct KeyboardRow {
    int x;
    int y;
    std::wstring_view fallback;
    std::array<UINT, 13> scanCodes;
};

// Set 1 scan codes identify the physical key positions. The fallback keeps every
// supported virtual key selectable even on layouts without all Latin letters.
constexpr KeyboardRow mainRows[]{
    {22, 104, L" 1234567890  ", {0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d}},
    {48, 122, L"QWERTYUIOP   ", {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x2b}},
    {51, 140, L"ASDFGHJKL  ", {0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28}},
    {52, 158, L" ZXCVBNM   ", {0x56, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35}},
};

bool isAlphanumeric(UINT key)
{
    return (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9');
}

bool completeLayout(HKL layout)
{
    std::bitset<256> keys;
    for (const auto& row : mainRows) {
        for (std::size_t index = 0; index < row.fallback.size(); ++index) {
            const auto key = MapVirtualKeyExW(row.scanCodes[index], MAPVK_VSC_TO_VK_EX, layout);
            if (isAlphanumeric(key))
                keys.set(key);
        }
    }
    return keys.count() == 36;
}

std::wstring inactiveLegend(UINT key, HKL layout)
{
    const auto character = LOWORD(MapVirtualKeyExW(key, MAPVK_VK_TO_CHAR, layout));
    return character > L' ' ? std::wstring(1, static_cast<wchar_t>(character)) : std::wstring{};
}

std::wstring keyLegend(UINT key)
{
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return std::to_wstring(key - VK_NUMPAD0);
    switch (key) {
    case VK_LEFT: return L"←";
    case VK_UP: return L"↑";
    case VK_RIGHT: return L"→";
    case VK_DOWN: return L"↓";
    case VK_ADD: return L"+";
    case VK_SUBTRACT: return L"−";
    default: return accessoryKeyLabel(key);
    }
}

class Keyboard {
public:
    explicit Keyboard(HWND window) : window_(window), instance_(GetModuleHandleW(nullptr)) {}

    bool build(HKL layout);

private:
    void add(UINT key, const wchar_t* inactive, int x, int y, int width = keyWidth, int height = keyHeight);

    HWND window_;
    HINSTANCE instance_;
    std::bitset<256> added_;
    bool complete_{true};
};

void Keyboard::add(UINT key, const wchar_t* inactive, int x, int y, int width, int height)
{
    const bool enabled = isAccessoryKey(key) && !added_.test(key);
    // Keep the full, distinct name for assistive technology. Paint a short legend on the keycap.
    const auto label = enabled ? accessoryKeyLabel(key) : std::wstring(inactive);
    RECT bounds{x, y, x + width, y + height};
    MapDialogRect(window_, &bounds);
    const auto id = static_cast<INT_PTR>(enabled ? firstKeyId + key : inactiveKeyId);
    const auto button = CreateWindowExW(0, L"Button", label.c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW |
        (enabled ? WS_TABSTOP : WS_DISABLED), bounds.left, bounds.top, bounds.right - bounds.left,
        bounds.bottom - bounds.top, window_, reinterpret_cast<HMENU>(id), instance_, nullptr);
    if (!button) {
        complete_ = false;
        return;
    }
    if (enabled)
        added_.set(key);
    const auto font = SendDlgItemMessageW(window_, IDCANCEL, WM_GETFONT, 0, 0);
    SendMessageW(button, WM_SETFONT, static_cast<WPARAM>(font), FALSE);
}

bool Keyboard::build(HKL layout)
{
    add(0, L"Esc", 22, 85, 22);
    for (UINT index = 0; index < 12; ++index) {
        add(VK_F1 + index, L"", 50 + static_cast<int>(index) * keyPitch + static_cast<int>(index / 4) * 4, 85);
    }

    const bool useLayout = completeLayout(layout);
    add(0, L"⌫", 243, 104, 25);
    add(0, L"Tab", 22, 122, 25);
    add(0, tr(Text::DlgKeyboardCapsLock), 22, 140, 28);
    add(0, tr(Text::DlgKeyboardEnter), 238, 140, 30);
    add(0, tr(Text::DlgKeyboardShift), 22, 158, 29);
    add(0, tr(Text::DlgKeyboardShift), 239, 158, 29);
    for (const auto& row : mainRows) {
        for (std::size_t index = 0; index < row.fallback.size(); ++index) {
            const auto mapped = MapVirtualKeyExW(row.scanCodes[index], MAPVK_VSC_TO_VK_EX, layout);
            const auto candidate = useLayout ? mapped : static_cast<UINT>(row.fallback[index]);
            const auto legend = inactiveLegend(mapped, layout);
            add(isAlphanumeric(candidate) ? candidate : 0, legend.c_str(),
                row.x + static_cast<int>(index) * keyPitch, row.y);
        }
    }
    add(0, L"Ctrl", 22, 176, 27);
    add(0, L"Win", 50, 176, 22);
    add(0, L"Alt", 73, 176, 22);
    add(VK_SPACE, L"", 96, 176, 100);
    add(0, L"AltGr", 197, 176, 24);
    add(0, L"Menu", 222, 176, 21);
    add(0, L"Ctrl", 244, 176, 24);

    add(0, L"Ins", 271, 104);
    add(0, tr(Text::DlgKeyboardHome), 288, 104);
    add(0, L"Pg↑", 305, 104);
    add(0, tr(Text::DlgKeyboardDelete), 271, 122);
    add(0, tr(Text::DlgKeyboardEnd), 288, 122);
    add(0, L"Pg↓", 305, 122);
    add(VK_UP, L"", 288, 158);
    add(VK_LEFT, L"", 271, 176);
    add(VK_DOWN, L"", 288, 176);
    add(VK_RIGHT, L"", 305, 176);

    add(0, tr(Text::DlgKeyboardNumLock), 328, 104);
    add(0, L"/", 345, 104);
    add(0, L"*", 362, 104);
    add(VK_SUBTRACT, L"", 379, 104);
    for (UINT row = 0; row < 3; ++row) {
        for (UINT column = 0; column < 3; ++column) {
            add(VK_NUMPAD7 - row * 3 + column, L"", 328 + static_cast<int>(column) * keyPitch,
                122 + static_cast<int>(row) * 18);
        }
    }
    add(VK_ADD, L"", 379, 122, keyWidth, 34);
    add(0, L"↵", 379, 158, keyWidth, 34);
    add(VK_NUMPAD0, L"", 328, 176, 33);
    add(0, L".", 362, 176);

    for (UINT key = 0; key < added_.size(); ++key) {
        if (isAccessoryKey(key) && !added_.test(key))
            return false;
    }
    return complete_;
}

void drawKey(const DRAWITEMSTRUCT& item, UINT key, UINT selected)
{
    const bool enabled = key != 0;
    const bool active = enabled && key == selected;
    const bool focused = enabled && (item.itemState & ODS_FOCUS) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool highContrast = ui::isHighContrast();
    const auto border = highContrast ? GetSysColor(active || focused ? COLOR_HIGHLIGHT : COLOR_WINDOWTEXT)
        : focused ? ui::yellow : active ? ui::cyan : ui::edge;
    const auto fill = highContrast ? GetSysColor(active || pressed ? COLOR_HIGHLIGHT : COLOR_WINDOW)
        : active || pressed ? RGB(15, 55, 60) : enabled ? ui::field : ui::panel;
    const auto text = highContrast ? GetSysColor(!enabled ? COLOR_GRAYTEXT
        : active || pressed ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)
        : !enabled ? RGB(111, 111, 111) : active ? ui::cyan : ui::ink;

    ui::fill(item.hDC, item.rcItem, border);
    auto labelBounds = item.rcItem;
    InflateRect(&labelBounds, -1, -1);
    ui::fill(item.hDC, labelBounds, fill);

    wchar_t inactive[24]{};
    const auto legend = enabled ? keyLegend(key)
        : std::wstring(inactive, static_cast<std::size_t>(GetWindowTextW(item.hwndItem, inactive, 24)));
    const auto saved = SaveDC(item.hDC);
    SelectObject(item.hDC, reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    DrawTextW(item.hDC, legend.c_str(), -1, &labelBounds, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    if (saved)
        RestoreDC(item.hDC, saved);
}

INT_PTR CALLBACK keyboardProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        SetWindowTextW(window, tr(Text::DlgKeyboardTitle));
        SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(Text::DlgKeyboardIntro));
        SetDlgItemTextW(window, IDC_KEYBOARD_NAVIGATION_LABEL, tr(Text::DlgKeyboardNavigation));
        SetDlgItemTextW(window, IDC_KEYBOARD_NUMPAD_LABEL, tr(Text::DlgKeyboardNumpad));
        SetDlgItemTextW(window, IDC_KEYBOARD_NUMLOCK_HINT, tr(Text::DlgKeypadNumLockHint));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, tr(Text::DlgKeyboardHeading), 0, 0, {});
        if (!Keyboard(window).build(GetKeyboardLayout(0))) {
            EndDialog(window, -1);
            return TRUE;
        }
        const auto current = GetDlgItem(window, firstKeyId + static_cast<int>(lParam));
        SetFocus(current ? current : GetDlgItem(window, IDCANCEL));
        return FALSE;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item.CtlID == inactiveKeyId) {
            drawKey(item, 0, 0);
            return TRUE;
        }
        if (item.CtlID >= firstKeyId && isAccessoryKey(item.CtlID - firstKeyId)) {
            drawKey(item, item.CtlID - firstKeyId, static_cast<UINT>(GetWindowLongPtrW(window, DWLP_USER)));
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        const auto id = LOWORD(wParam);
        if (id == IDCANCEL) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (HIWORD(wParam) == BN_CLICKED && id >= firstKeyId && isAccessoryKey(id - firstKeyId)) {
            EndDialog(window, id - firstKeyId);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

} // namespace

std::optional<unsigned int> chooseAccessoryKey(HWND owner, unsigned int currentKey)
{
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ACCESSORY_KEYBOARD),
        owner, keyboardProc, isAccessoryKey(currentKey) ? currentKey : 0);
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgKeyboardOpenError), L"Chrome Axis", MB_OK | MB_ICONERROR);
    if (result > 0 && isAccessoryKey(static_cast<unsigned int>(result)))
        return static_cast<unsigned int>(result);
    return std::nullopt;
}

} // namespace chrome_axis
