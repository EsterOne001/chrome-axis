#include "dialog_theme.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "ui_theme.hpp"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace chrome_axis {
namespace {

constexpr UINT_PTR dialogSubclassId = 1;
constexpr UINT_PTR controlSubclassId = 2;

bool classIs(HWND window, const wchar_t* name)
{
    wchar_t className[32]{};
    return GetClassNameW(window, className, static_cast<int>(std::size(className))) != 0
        && lstrcmpiW(className, name) == 0;
}

RECT controlBounds(HWND dialog, int id)
{
    RECT bounds{};
    if (const auto control = GetDlgItem(dialog, id); control && GetWindowRect(control, &bounds)) {
        POINT corners[]{{bounds.left, bounds.top}, {bounds.right, bounds.bottom}};
        MapWindowPoints(HWND_DESKTOP, dialog, corners, static_cast<UINT>(std::size(corners)));
        bounds = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    }
    return bounds;
}

std::wstring itemText(HWND control, WPARAM index, UINT sizeMessage, UINT textMessage)
{
    const auto length = SendMessageW(control, sizeMessage, index, 0);
    if (length <= 0)
        return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(control, textMessage, index, reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(length));
    return text;
}

class ThemeState {
public:
    ThemeState(HWND dialog, const wchar_t* subtitle, int statusId, int listId,
               std::initializer_list<int> primaryButtons)
        : dialog_(dialog), subtitle_(subtitle), statusId_(statusId), listId_(listId),
          primaryButtons_(primaryButtons), backgroundBrush_(CreateSolidBrush(ui::background)),
          panelBrush_(CreateSolidBrush(ui::panel)), fieldBrush_(CreateSolidBrush(ui::field)),
          edgeBrush_(CreateSolidBrush(ui::edge))
    {
    }

    ~ThemeState()
    {
        for (const auto font : fonts_)
            DeleteObject(font);
        DeleteObject(backgroundBrush_);
        DeleteObject(panelBrush_);
        DeleteObject(fieldBrush_);
        DeleteObject(edgeBrush_);
    }

    void initialise();
    LRESULT handle(HWND window, UINT message, WPARAM wp, LPARAM lp);
    LRESULT handleControl(HWND window, UINT message, WPARAM wp, LPARAM lp);

private:
    void updateDpi();
    void paintScene(HDC dc) const;
    void paintButton(HWND window, HDC dc) const;
    void paintCombo(HWND window, HDC dc) const;
    void paintListItem(const DRAWITEMSTRUCT& item) const;
    HBRUSH controlColor(HDC dc, HWND control, UINT message) const;

    int px(int value) const { return ui::px(value, dpi_); }

    HWND dialog_{};
    std::wstring subtitle_;
    int statusId_{};
    int listId_{};
    std::vector<int> primaryButtons_;
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    std::array<HFONT, 3> fonts_{};
    ui::Icon logo_;
    HBRUSH backgroundBrush_{};
    HBRUSH panelBrush_{};
    HBRUSH fieldBrush_{};
    HBRUSH edgeBrush_{};
};

LRESULT CALLBACK controlProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, controlProc, id);
        return DefSubclassProc(window, message, wp, lp);
    }
    return reinterpret_cast<ThemeState*>(data)->handleControl(window, message, wp, lp);
}

LRESULT CALLBACK themeProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    auto* state = reinterpret_cast<ThemeState*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, themeProc, id);
        const auto result = DefSubclassProc(window, message, wp, lp);
        delete state;
        return result;
    }
    return state->handle(window, message, wp, lp);
}

void ThemeState::initialise()
{
    for (const int id : {IDC_MODAL_FORM, IDC_MODAL_RESULTS})
        ShowWindow(GetDlgItem(dialog_, id), SW_HIDE);
    if (const auto list = GetDlgItem(dialog_, listId_)) {
        SetWindowLongPtrW(list, GWL_STYLE, GetWindowLongPtrW(list, GWL_STYLE) & ~static_cast<LONG_PTR>(WS_BORDER));
        SetWindowLongPtrW(list, GWL_EXSTYLE, GetWindowLongPtrW(list, GWL_EXSTYLE)
            & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(list, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    EnumChildWindows(dialog_, [](HWND control, LPARAM data) -> BOOL {
        if (classIs(control, L"Button") || classIs(control, L"ComboBox"))
            SetWindowSubclass(control, controlProc, controlSubclassId, static_cast<DWORD_PTR>(data));
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    updateDpi();
}

void ThemeState::updateDpi()
{
    if (const auto dpi = GetDpiForWindow(dialog_))
        dpi_ = dpi;
    const auto oldFonts = fonts_;
    fonts_ = ui::makeFonts(dialog_, dpi_);
    logo_ = ui::loadLogo(GetModuleHandleW(nullptr), px(ui::headerLogoSize));
    EnumChildWindows(dialog_, [](HWND control, LPARAM data) -> BOOL {
        const auto& theme = *reinterpret_cast<const ThemeState*>(data);
        const bool heading = GetDlgCtrlID(control) == IDC_MODAL_LIST_HEADING;
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(theme.fonts_[heading ? 1 : 0]), FALSE);
        if (classIs(control, L"ComboBox"))
            SendMessageW(control, CB_SETITEMHEIGHT, 0, theme.px(24));
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    SendDlgItemMessageW(dialog_, listId_, LB_SETITEMHEIGHT, 0, px(24));
    for (const auto font : oldFonts)
        DeleteObject(font);
    RedrawWindow(dialog_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void ThemeState::paintScene(HDC dc) const
{
    RECT client{};
    GetClientRect(dialog_, &client);
    const bool highContrast = ui::isHighContrast();
    ui::fill(dc, client, highContrast ? GetSysColor(COLOR_WINDOW) : ui::background);
    ui::paintHeader(dc, dpi_, client.right, 24, client.right * 3 / 4, logo_.get(), fonts_[0], fonts_[2],
                    subtitle_.c_str());
    const auto border = highContrast ? GetSysColor(COLOR_WINDOWTEXT) : ui::edge;
    for (const int id : {IDC_MODAL_FORM, IDC_MODAL_RESULTS}) {
        const auto card = controlBounds(dialog_, id);
        if (IsRectEmpty(&card))
            continue;
        ui::cutPanel(dc, card, px(13), highContrast ? GetSysColor(COLOR_WINDOW) : ui::panel, border);
        ui::fill(dc, {card.left, card.top + px(12), card.left + px(2), card.top + px(33)},
                 highContrast ? GetSysColor(COLOR_HIGHLIGHT) : ui::cyan);
        if (id == IDC_MODAL_RESULTS) {
            const auto rule = card.bottom + px(9);
            ui::fill(dc, {card.left, rule, card.right, rule + 1}, border);
        }
    }
    auto outline = controlBounds(dialog_, listId_);
    if (!IsRectEmpty(&outline)) {
        InflateRect(&outline, 1, 1);
        FrameRect(dc, &outline, highContrast ? GetSysColorBrush(COLOR_WINDOWTEXT) : edgeBrush_);
    }
}

void ThemeState::paintButton(HWND window, HDC dc) const
{
    DRAWITEMSTRUCT item{};
    item.CtlType = ODT_BUTTON;
    item.CtlID = static_cast<UINT>(GetDlgCtrlID(window));
    item.hwndItem = window;
    item.hDC = dc;
    GetClientRect(window, &item.rcItem);
    const auto state = SendMessageW(window, BM_GETSTATE, 0, 0);
    const auto uiState = SendMessageW(window, WM_QUERYUISTATE, 0, 0);
    if (!IsWindowEnabled(window))
        item.itemState |= ODS_DISABLED;
    if (state & BST_PUSHED)
        item.itemState |= ODS_SELECTED;
    if (GetFocus() == window)
        item.itemState |= ODS_FOCUS;
    if (uiState & UISF_HIDEFOCUS)
        item.itemState |= ODS_NOFOCUSRECT;
    if (uiState & UISF_HIDEACCEL)
        item.itemState |= ODS_NOACCEL;
    const bool primary = std::ranges::find(primaryButtons_, static_cast<int>(item.CtlID)) != primaryButtons_.end();
    const auto footer = controlBounds(dialog_, IDC_MODAL_RESULTS).bottom;
    const auto box = controlBounds(dialog_, static_cast<int>(item.CtlID));
    ui::paintAction(item, fonts_[0], dpi_, box.top >= footer ? ui::background : ui::panel, primary);
}

void ThemeState::paintCombo(HWND window, HDC dc) const
{
    RECT rect{};
    GetClientRect(window, &rect);
    if (IsRectEmpty(&rect))
        return;
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const bool focused = GetFocus() == window || SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != 0;
    const bool highContrast = ui::isHighContrast();
    const auto border = highContrast ? GetSysColor(focused ? COLOR_HIGHLIGHT : COLOR_WINDOWTEXT)
        : enabled && focused ? ui::cyan : ui::edge;
    const auto foreground = highContrast ? GetSysColor(enabled ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT)
        : enabled ? ui::ink : ui::secondary;
    ui::fill(dc, rect, border);
    InflateRect(&rect, -1, -1);
    ui::fill(dc, rect, highContrast ? GetSysColor(COLOR_WINDOW) : ui::field);
    const auto arrowX = rect.right - px(13);
    const auto arrowY = (rect.top + rect.bottom) / 2;
    const POINT arrow[]{{arrowX - px(4), arrowY - px(2)}, {arrowX + px(4), arrowY - px(2)}, {arrowX, arrowY + px(2)}};
    const auto arrowColor = highContrast || !enabled ? foreground : ui::cyan;
    ui::drawPolygon(dc, arrow, arrowColor, arrowColor);

    const auto selected = SendMessageW(window, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR)
        return;
    const auto label = itemText(window, static_cast<WPARAM>(selected), CB_GETLBTEXTLEN, CB_GETLBTEXT);
    rect.left += px(8);
    rect.right = arrowX - px(10);
    const auto saved = SaveDC(dc);
    SelectObject(dc, fonts_[0]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, foreground);
    DrawTextW(dc, label.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (saved)
        RestoreDC(dc, saved);
}

void ThemeState::paintListItem(const DRAWITEMSTRUCT& item) const
{
    const bool highContrast = ui::isHighContrast();
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const auto background = highContrast ? GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW)
        : selected ? ui::edge : ui::field;
    const auto foreground = highContrast ? GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)
        : selected ? ui::cyan : ui::ink;
    ui::fill(item.hDC, item.rcItem, background);
    if (item.itemID != static_cast<UINT>(-1)) {
        const auto label = itemText(item.hwndItem, item.itemID, LB_GETTEXTLEN, LB_GETTEXT);
        RECT rect = item.rcItem;
        InflateRect(&rect, -px(8), 0);
        const auto saved = SaveDC(item.hDC);
        SelectObject(item.hDC, fonts_[0]);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, foreground);
        DrawTextW(item.hDC, label.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (saved)
            RestoreDC(item.hDC, saved);
    }
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT))
        DrawFocusRect(item.hDC, &item.rcItem);
}

HBRUSH ThemeState::controlColor(HDC dc, HWND control, UINT message) const
{
    const bool highContrast = ui::isHighContrast();
    if (highContrast) {
        SetTextColor(dc, GetSysColor(IsWindowEnabled(control) ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return GetSysColorBrush(COLOR_WINDOW);
    }
    if (message == WM_CTLCOLORDLG) {
        SetBkColor(dc, ui::background);
        return backgroundBrush_;
    }
    if (message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLOREDIT) {
        SetTextColor(dc, ui::ink);
        SetBkColor(dc, ui::field);
        return fieldBrush_;
    }
    const auto id = GetDlgCtrlID(control);
    SetTextColor(dc, id == IDC_MODAL_LIST_HEADING ? ui::yellow
        : id == statusId_ || id == IDC_MODAL_INTRO ? ui::secondary : ui::ink);
    SetBkColor(dc, ui::panel);
    SetBkMode(dc, OPAQUE);
    return panelBrush_;
}

LRESULT ThemeState::handle(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        paintScene(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paintScene(reinterpret_cast<HDC>(wp));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
        return reinterpret_cast<LRESULT>(controlColor(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), message));
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_COMBOBOX) {
            ui::paintComboItem(item, fonts_[0], dpi_);
            return TRUE;
        }
        if (item.CtlType == ODT_LISTBOX && item.CtlID == static_cast<UINT>(listId_)) {
            paintListItem(item);
            return TRUE;
        }
        break;
    }
    case WM_MEASUREITEM: {
        auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_COMBOBOX || item.CtlID == static_cast<UINT>(listId_)) {
            item.itemHeight = static_cast<UINT>(px(24));
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (lp && (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_DROPDOWN || HIWORD(wp) == CBN_CLOSEUP)
                && classIs(reinterpret_cast<HWND>(lp), L"ComboBox"))
            InvalidateRect(reinterpret_cast<HWND>(lp), nullptr, FALSE);
        break;
    case WM_DPICHANGED: {
        const auto result = DefSubclassProc(window, message, wp, lp);
        updateDpi();
        return result;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        break;
    }
    return DefSubclassProc(window, message, wp, lp);
}

LRESULT ThemeState::handleControl(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const auto dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wp);
        if (classIs(window, L"Button"))
            paintButton(window, dc);
        else
            paintCombo(window, dc);
        if (message == WM_PAINT)
            EndPaint(window, &paint);
        return 0;
    }
    const auto result = DefSubclassProc(window, message, wp, lp);
    switch (message) {
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SETTEXT:
    case WM_UPDATEUISTATE:
    case WM_SETFONT:
    case BM_SETSTATE:
    case BM_SETSTYLE:
    case CB_RESETCONTENT:
    case CB_SETCURSEL:
    case CB_SELECTSTRING:
    case CB_SHOWDROPDOWN:
        InvalidateRect(window, nullptr, FALSE);
        break;
    }
    return result;
}

} // namespace

void DialogTheme::attach(HWND dialog, const wchar_t* subtitle, int statusId, int listId,
                         std::initializer_list<int> primaryButtons)
{
    auto theme = std::make_unique<ThemeState>(dialog, subtitle, statusId, listId, primaryButtons);
    if (SetWindowSubclass(dialog, themeProc, dialogSubclassId, reinterpret_cast<DWORD_PTR>(theme.get())))
        theme.release()->initialise();
}

} // namespace chrome_axis
