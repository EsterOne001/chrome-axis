#pragma once

#include <windows.h>

#include <array>
#include <memory>
#include <type_traits>

namespace chrome_axis::ui {

inline constexpr WORD logoResource = 103;
inline constexpr int headerLogoSize = 56;
inline constexpr COLORREF background = RGB(0, 0, 0);
inline constexpr COLORREF panel = RGB(15, 15, 15);
inline constexpr COLORREF field = RGB(28, 28, 28);
inline constexpr COLORREF ink = RGB(255, 255, 255);
inline constexpr COLORREF secondary = RGB(203, 203, 203);
inline constexpr COLORREF edge = RGB(76, 76, 76);
inline constexpr COLORREF yellow = RGB(252, 238, 10);
inline constexpr COLORREF cyan = RGB(0, 240, 255);
inline constexpr COLORREF red = RGB(254, 16, 56);

inline int px(int size, UINT dpi) { return MulDiv(size, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI); }

void fill(HDC dc, RECT rect, COLORREF color);

template<std::size_t count>
void drawPolygon(HDC dc, const POINT (&points)[count], COLORREF color, COLORREF border)
{
    const auto brush = CreateSolidBrush(color);
    const auto pen = CreatePen(PS_SOLID, 1, border);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    Polygon(dc, points, static_cast<int>(count));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void cutPanel(HDC dc, RECT rect, LONG cut, COLORREF color, COLORREF border);
bool isHighContrast();

struct IconDeleter {
    void operator()(HICON icon) const noexcept { DestroyIcon(icon); }
};
using Icon = std::unique_ptr<std::remove_pointer_t<HICON>, IconDeleter>;

Icon loadLogo(HINSTANCE instance, int size);
std::array<HFONT, 3> makeFonts(HWND window, UINT dpi);

void paintHeader(HDC dc, UINT dpi, LONG width, int textLeft, LONG plateEdge, HICON logo,
                 HFONT body, HFONT wordmark, const wchar_t* subtitle);
enum class Flag { france, unitedKingdom };
bool shapeLanguageButton(HWND button, int size, Flag flag);
void paintLanguageButton(const DRAWITEMSTRUCT& item, UINT dpi, Flag flag, bool active);
void paintAction(const DRAWITEMSTRUCT& item, HFONT font, UINT dpi, COLORREF base, bool primary, bool stop = false);
void paintComboItem(const DRAWITEMSTRUCT& item, HFONT font, UINT dpi);

} // namespace chrome_axis::ui
