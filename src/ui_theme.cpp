#include "ui_theme.hpp"

#include <cwchar>
#include <initializer_list>
#include <string>

namespace chrome_axis::ui {
namespace {

HANDLE loadEmbeddedFont(WORD id)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    const auto data = resource ? LoadResource(module, resource) : nullptr;
    DWORD count{};
    return data ? AddFontMemResourceEx(LockResource(data), SizeofResource(module, resource), nullptr, &count) : nullptr;
}

struct EmbeddedFonts {
    HANDLE heading = loadEmbeddedFont(101);
    HANDLE body = loadEmbeddedFont(102);

    ~EmbeddedFonts()
    {
        if (heading)
            RemoveFontMemResourceEx(heading);
        if (body)
            RemoveFontMemResourceEx(body);
    }
};

const wchar_t* installedFace(HDC dc, std::initializer_list<const wchar_t*> faces)
{
    if (!dc)
        return nullptr;
    for (const auto* face : faces) {
        LOGFONTW filter{};
        filter.lfCharSet = DEFAULT_CHARSET;
        if (wcsncpy_s(filter.lfFaceName, face, _TRUNCATE) != 0)
            continue;
        bool found = false;
        EnumFontFamiliesExW(dc, &filter, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM result) -> int {
            *reinterpret_cast<bool*>(result) = true;
            return 0;
        }, reinterpret_cast<LPARAM>(&found), 0);
        if (found)
            return face;
    }
    return nullptr;
}

} // namespace

void fill(HDC dc, RECT rect, COLORREF color)
{
    const auto brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void cutPanel(HDC dc, RECT rect, LONG cut, COLORREF color, COLORREF border)
{
    const POINT points[]{
        {rect.left, rect.top}, {rect.right - 1, rect.top}, {rect.right - 1, rect.bottom - cut - 1},
        {rect.right - cut - 1, rect.bottom - 1}, {rect.left, rect.bottom - 1}
    };
    drawPolygon(dc, points, color, border);
}

bool isHighContrast()
{
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

Icon loadLogo(HINSTANCE instance, int size)
{
    return Icon(static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(logoResource), IMAGE_ICON, size, size, 0)));
}

std::array<HFONT, 3> makeFonts(HWND window, UINT dpi)
{
    static const EmbeddedFonts embedded;
    const auto dc = GetDC(window);
    const auto* blenderBook = installedFace(dc, {L"Blender Pro Book", L"Blender Pro", L"Blender Book", L"Blender"});
    const auto* blenderBold = installedFace(dc, {L"Blender Pro Bold", L"Blender Bold", L"Blender Pro", L"Blender"});
    const auto* refinery = installedFace(dc, {L"Refinery 25 SemiBold", L"Refinery-25 SemiBold", L"Refinery 25", L"Refinery-25"});
    if (dc)
        ReleaseDC(window, dc);
    const auto makeFont = [dpi](int size, int weight, const wchar_t* face) {
        return CreateFontW(-px(size, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    const auto* body = blenderBook ? blenderBook : embedded.body ? L"Barlow Semi Condensed Medium" : L"Segoe UI";
    const auto* display = embedded.heading ? L"Barlow Condensed SemiBold" : L"Segoe UI";
    return {
        makeFont(16, FW_NORMAL, body),
        makeFont(22, blenderBold ? FW_BOLD : FW_NORMAL, blenderBold ? blenderBold : display),
        makeFont(42, refinery ? FW_SEMIBOLD : FW_NORMAL, refinery ? refinery : display)
    };
}

void paintHeader(HDC dc, UINT dpi, LONG width, int textLeft, LONG plateEdge, HICON logo,
                 HFONT body, HFONT wordmark, const wchar_t* subtitle)
{
    const auto scale = [dpi](int size) { return px(size, dpi); };
    const bool highContrast = isHighContrast();
    const auto header = highContrast ? GetSysColor(COLOR_HIGHLIGHT) : yellow;
    const POINT hero[]{
        {0, 0}, {plateEdge, 0}, {plateEdge - scale(35), scale(84)}, {0, scale(84)}
    };
    drawPolygon(dc, hero, header, header);
    fill(dc, {0, scale(84), width, scale(88)}, header);
    if (!highContrast)
        fill(dc, {plateEdge - scale(35), scale(84), plateEdge - scale(11), scale(87)}, background);

    if (logo) {
        DrawIconEx(dc, scale(textLeft), scale(14), logo, scale(headerLogoSize), scale(headerLogoSize), 0, nullptr, DI_NORMAL);
        textLeft += headerLogoSize + 16;
    }
    const auto old = SelectObject(dc, wordmark);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : background);
    RECT heading{scale(textLeft), scale(6), plateEdge - scale(18), scale(53)};
    DrawTextW(dc, L"CHROME AXIS", -1, &heading, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, body);
    RECT tagline{scale(textLeft + 1), scale(54), plateEdge - scale(34), scale(77)};
    DrawTextW(dc, subtitle, -1, &tagline, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

void paintAction(const DRAWITEMSTRUCT& item, HFONT font, UINT dpi, COLORREF base, bool primary, bool stop)
{
    const bool highContrast = isHighContrast();
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    fill(item.hDC, item.rcItem, highContrast ? GetSysColor(COLOR_WINDOW) : base);
    const auto face = highContrast ? GetSysColor(pressed ? COLOR_HIGHLIGHT : COLOR_BTNFACE)
        : disabled ? field : stop ? red : primary ? cyan : pressed ? edge : background;
    const auto border = highContrast ? GetSysColor(COLOR_BTNTEXT)
        : disabled ? edge : primary ? face : cyan;
    const auto& box = item.rcItem;
    const LONG left = box.left, right = box.right - 1, top = box.top, bottom = box.bottom - 1;
    const LONG cut = px(11, dpi), notch = px(7, dpi);
    const LONG mid = (top + bottom) / 2;
    const POINT shape[]{
        {left, top}, {right, top}, {right, bottom - cut}, {right - cut, bottom}, {left, bottom},
        {left, mid + 1}, {left + notch, mid + 1}, {left + notch, mid - 1}, {left, mid - 1}
    };
    drawPolygon(item.hDC, shape, face, border);

    const auto previous = SaveDC(item.hDC);
    SetTextColor(item.hDC, highContrast ? GetSysColor(disabled ? COLOR_GRAYTEXT : pressed ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT)
        : disabled ? secondary : stop ? ink : primary ? background : cyan);
    SetBkMode(item.hDC, TRANSPARENT);
    const auto oldFont = SelectObject(item.hDC, font);
    wchar_t text[80]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    auto rect = item.rcItem;
    InflateRect(&rect, -px(8, dpi), 0);
    const UINT prefix = item.itemState & ODS_NOACCEL ? DT_HIDEPREFIX : 0;
    DrawTextW(item.hDC, text, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | prefix);
    SelectObject(item.hDC, oldFont);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        auto focus = item.rcItem;
        InflateRect(&focus, -px(4, dpi), -px(4, dpi));
        DrawFocusRect(item.hDC, &focus);
    }
    if (previous)
        RestoreDC(item.hDC, previous);
}

void paintComboItem(const DRAWITEMSTRUCT& item, HFONT font, UINT dpi)
{
    const bool highContrast = isHighContrast();
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected = !disabled && (item.itemState & ODS_SELECTED) != 0;
    const auto bg = highContrast ? GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW)
        : selected ? edge : field;
    fill(item.hDC, item.rcItem, bg);
    if (item.itemID == static_cast<UINT>(-1))
        return;
    const auto length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
    if (length == CB_ERR)
        return;
    std::wstring labelText(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(labelText.data()));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, highContrast ? GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)
                                       : disabled ? secondary : ink);
    const auto oldFont = SelectObject(item.hDC, font);
    RECT rect = item.rcItem;
    rect.left += px(5, dpi);
    DrawTextW(item.hDC, labelText.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT))
        DrawFocusRect(item.hDC, &item.rcItem);
}

} // namespace chrome_axis::ui
