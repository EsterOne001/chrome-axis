#include "calibration_guide.hpp"

#include "ui_theme.hpp"
#include "app_text.hpp"

#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace chrome_axis::ui {
namespace {

constexpr int diagramWidth = 200;
constexpr int diagramHeight = 142;

struct Diagram {
    LONG left;
    LONG top;
    int width;

    LONG scale(LONG value) const { return MulDiv(value, width, diagramWidth); }
    POINT point(LONG x, LONG y) const { return {left + scale(x), top + scale(y)}; }

    template<std::size_t count>
    void line(HDC dc, const POINT (&shape)[count]) const
    {
        POINT points[count];
        for (std::size_t i = 0; i < count; ++i)
            points[i] = point(shape[i].x, shape[i].y);
        Polyline(dc, points, static_cast<int>(count));
    }

    void ellipse(HDC dc, LONG x, LONG y, LONG rx, LONG ry) const
    {
        const auto from = point(x - rx, y - ry);
        const auto to = point(x + rx, y + ry);
        Ellipse(dc, from.x, from.y, to.x, to.y);
    }
};

class CalibrationArtwork {
    struct Image {
        std::unique_ptr<IStream, decltype([](IStream* stream) { stream->Release(); })> stream;
        std::unique_ptr<Gdiplus::Bitmap> bitmap;

        void load(WORD id)
        {
            const auto module = GetModuleHandleW(nullptr);
            const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
            if (!resource)
                return;
            const auto data = LoadResource(module, resource);
            const auto bytes = data ? static_cast<const BYTE*>(LockResource(data)) : nullptr;
            if (!bytes)
                return;
            stream.reset(SHCreateMemStream(bytes, SizeofResource(module, resource)));
            if (stream) {
                auto decoded = std::make_unique<Gdiplus::Bitmap>(stream.get());
                if (decoded->GetLastStatus() == Gdiplus::Ok)
                    bitmap = std::move(decoded);
            }
        }
    };

public:
    CalibrationArtwork()
    {
        const Gdiplus::GdiplusStartupInput options;
        if (Gdiplus::GdiplusStartup(&token_, &options, nullptr) == Gdiplus::Ok) {
            wheel_.load(104);
            pedal_.load(105);
        }
    }

    ~CalibrationArtwork()
    {
        pedal_.bitmap.reset();
        wheel_.bitmap.reset();
        if (token_)
            Gdiplus::GdiplusShutdown(token_);
    }

    Gdiplus::Bitmap* image(bool pedal) const { return (pedal ? pedal_ : wheel_).bitmap.get(); }

private:
    ULONG_PTR token_{};
    Image wheel_;
    Image pedal_;
};

constexpr float radians = 0.01745329252f;

POINT rotateWheelPoint(const Diagram& diagram, LONG x, LONG y, float angle)
{
    const auto cosine = std::cos(angle * radians);
    const auto sine = std::sin(angle * radians);
    return diagram.point(100 + std::lround(x * cosine - y * sine),
                         76 + std::lround(x * sine + y * cosine));
}

bool drawWheelArtwork(HDC dc, const Diagram& diagram, Gdiplus::Bitmap& image, float angle)
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    POINT origin{};
    GetViewportOrgEx(dc, &origin);
    const auto hub = diagram.point(100, 76);
    graphics.TranslateTransform(static_cast<float>(hub.x + origin.x), static_cast<float>(hub.y + origin.y));
    graphics.RotateTransform(angle);
    const int size = diagram.scale(118);
    return graphics.DrawImage(&image, -size / 2, -size / 2, size, size) == Gdiplus::Ok;
}

void paintWheelOutline(HDC dc, const Diagram& diagram, float angle, COLORREF fg, COLORREF accent)
{
    const auto state = SaveDC(dc);
    if (!state)
        return;
    const auto outline = CreatePen(PS_SOLID, std::max(1L, diagram.scale(3)), fg);
    const auto mark = CreatePen(PS_SOLID, std::max(1L, diagram.scale(4)), accent);
    if (outline && mark) {
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        SelectObject(dc, outline);
        diagram.ellipse(dc, 100, 76, 42, 42);
        diagram.ellipse(dc, 100, 76, 8, 8);
        const POINT spokes[][2]{
            {rotateWheelPoint(diagram, -37, -14, angle), rotateWheelPoint(diagram, -8, -2, angle)},
            {rotateWheelPoint(diagram, 37, -14, angle), rotateWheelPoint(diagram, 8, -2, angle)},
            {rotateWheelPoint(diagram, 0, 9, angle), rotateWheelPoint(diagram, 0, 40, angle)}
        };
        for (const auto& spoke : spokes)
            Polyline(dc, spoke, 2);
        SelectObject(dc, mark);
        const POINT stripe[]{rotateWheelPoint(diagram, 0, -37, angle), rotateWheelPoint(diagram, 0, -46, angle)};
        Polyline(dc, stripe, 2);
    }
    RestoreDC(dc, state);
    DeleteObject(mark);
    DeleteObject(outline);
}

void paintWheelCue(HDC dc, const Diagram& diagram, int turn, COLORREF accent, COLORREF border)
{
    if (turn == 0) {
        const POINT mark[]{
            diagram.point(90, 10), diagram.point(110, 10), diagram.point(100, 22)
        };
        drawPolygon(dc, mark, accent, border);
        return;
    }

    // A tapered arc outside the rim leads into a solid arrowhead.
    constexpr int segments = 12;
    const auto point = [&](float angle, float radius) {
        const float a = angle * radians;
        return diagram.point(100 + turn * std::lround(std::sin(a) * radius),
            76 - std::lround(std::cos(a) * radius));
    };
    POINT arrow[2 * (segments + 1) + 3]{};
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / segments;
        const float angle = 18.0f + 42.0f * t;
        const float halfWidth = 1.0f + 3.0f * t;
        arrow[i] = point(angle, 61.0f + halfWidth);
        arrow[2 * segments + 4 - i] = point(angle, 61.0f - halfWidth);
    }
    arrow[segments + 1] = point(60, 72);
    arrow[segments + 2] = point(79, 61);
    arrow[segments + 3] = point(60, 50);
    drawPolygon(dc, arrow, accent, border);
}

void paintPedalCue(HDC dc, const Diagram& diagram, LONG x, bool pressing, COLORREF accent, COLORREF border)
{
    const auto y = [pressing](LONG offset) { return pressing ? 39 + offset : 85 - offset; };
    const POINT arrow[]{
        diagram.point(x - 3, y(0)), diagram.point(x + 3, y(0)), diagram.point(x + 3, y(30)),
        diagram.point(x + 9, y(30)), diagram.point(x, y(46)),
        diagram.point(x - 9, y(30)), diagram.point(x - 3, y(30))
    };
    drawPolygon(dc, arrow, accent, border);
}

void paintPedal(HDC dc, const Diagram& diagram, LONG x, Gdiplus::Bitmap* image, float pressure,
    bool active, const wchar_t* label, HFONT font, bool highContrast)
{
    const auto frame = [&](LONG top, LONG bottom) {
        const auto start = diagram.point(x - 42, top);
        const auto end = diagram.point(x + 42, bottom);
        return RECT{start.x, start.y, end.x, end.y};
    };
    const COLORREF fg = highContrast ? GetSysColor(COLOR_WINDOWTEXT) : active ? yellow : secondary;
    const COLORREF line = highContrast ? fg : active ? yellow : edge;
    cutPanel(dc, frame(10, 132), diagram.scale(8), highContrast ? GetSysColor(COLOR_WINDOW) : panel, line);
    const auto top = 22L + std::lround(pressure * 31.0f);
    const auto height = 65L - std::lround(pressure * 18.0f);
    const auto plateStart = diagram.point(x - (x < 100 ? 26 : 22), top);
    const auto plateEnd = diagram.point(x + (x < 100 ? 14 : 10), top + height);
    const RECT plate{plateStart.x, plateStart.y, plateEnd.x, plateEnd.y};
    const auto mountStart = diagram.point(x - 22, 100);
    const auto mountEnd = diagram.point(x + 10, 104);
    fill(dc, {mountStart.x, mountStart.y, mountEnd.x, mountEnd.y}, line);

    bool drawn = false;
    if (image) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        POINT origin{};
        GetViewportOrgEx(dc, &origin);
        const Gdiplus::Rect bounds(plate.left + origin.x, plate.top + origin.y,
            plate.right - plate.left, plate.bottom - plate.top);
        const float opacity = active ? 1.0f : 0.48f;
        const Gdiplus::ColorMatrix tone = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0},
            {0, 0, 0, opacity, 0}, {0, 0, 0, 0, 1}}};
        Gdiplus::ImageAttributes attributes;
        attributes.SetColorMatrix(&tone);
        drawn = graphics.DrawImage(image, bounds, 0, 0, static_cast<int>(image->GetWidth()),
            static_cast<int>(image->GetHeight()), Gdiplus::UnitPixel, &attributes) == Gdiplus::Ok;
    }
    if (!drawn) {
        cutPanel(dc, plate, diagram.scale(5), highContrast ? GetSysColor(COLOR_WINDOW) : field, fg);
        for (LONG i = 1; i < 5; ++i) {
            const LONG y = plate.top + (plate.bottom - plate.top) * i / 5;
            fill(dc, {plate.left + diagram.scale(6), y, plate.right - diagram.scale(6), y + 1}, fg);
        }
    }

    const auto state = SaveDC(dc);
    if (state) {
        SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        auto text = frame(108, 130);
        DrawTextW(dc, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RestoreDC(dc, state);
    }
}

} // namespace

void paintCalibrationPose(HDC dc, const RECT& rect, UINT dpi, CalibrationPose pose, float motion, HFONT font)
{
    const bool highContrast = isHighContrast();
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const auto scaleWidth = std::min({width, MulDiv(height, diagramWidth, diagramHeight), px(diagramWidth, dpi)});
    if (scaleWidth < 24)
        return;

    const auto scaleHeight = MulDiv(scaleWidth, diagramHeight, diagramWidth);
    const Diagram diagram{rect.left + (width - scaleWidth) / 2, rect.top + (height - scaleHeight) / 2, scaleWidth};
    const auto foreground = highContrast ? GetSysColor(COLOR_WINDOWTEXT) : secondary;
    const auto accent = highContrast ? foreground : cyan;
    const auto arrowEdge = highContrast ? foreground : RGB(10, 104, 110);
    fill(dc, rect, highContrast ? GetSysColor(COLOR_WINDOW) : panel);
    cutPanel(dc, {diagram.left, diagram.top, diagram.left + scaleWidth, diagram.top + scaleHeight},
             diagram.scale(12), highContrast ? GetSysColor(COLOR_WINDOW) : field, highContrast ? foreground : edge);

    static const CalibrationArtwork artwork;
    const float phase = std::clamp(motion, 0.0f, 1.0f);
    const bool throttle = pose == CalibrationPose::throttleRest || pose == CalibrationPose::throttlePressed;
    const bool brake = pose == CalibrationPose::brakeRest || pose == CalibrationPose::brakePressed;
    if (throttle || brake) {
        const bool pressing = pose == CalibrationPose::throttlePressed || pose == CalibrationPose::brakePressed;
        const float pressure = pressing ? phase : 1.0f - phase;
        auto image = highContrast ? nullptr : artwork.image(true);
        paintPedal(dc, diagram, 52, image, brake ? pressure : 0.0f, brake, tr(Text::mainBrake), font, highContrast);
        paintPedal(dc, diagram, 148, image, throttle ? pressure : 0.0f, throttle, tr(Text::mainThrottle), font, highContrast);
        paintPedalCue(dc, diagram, brake ? 79 : 175, pressing, accent, arrowEdge);
        return;
    }

    const int turn = pose == CalibrationPose::wheelLeft ? -1 : pose == CalibrationPose::wheelRight ? 1 : 0;
    const float angle = turn * 58.0f * phase;
    auto image = highContrast ? nullptr : artwork.image(false);
    if (!image || !drawWheelArtwork(dc, diagram, *image, angle))
        paintWheelOutline(dc, diagram, angle, foreground, highContrast ? foreground : yellow);
    paintWheelCue(dc, diagram, turn, accent, arrowEdge);
}

void paintCalibrationProgress(HDC dc, const RECT& rect, UINT dpi, std::size_t completed, std::size_t total)
{
    const auto highContrast = isHighContrast();
    const auto base = highContrast ? GetSysColor(COLOR_WINDOW) : panel;
    fill(dc, rect, base);
    if (total == 0)
        return;

    const int width = rect.right - rect.left;
    if (width <= 0 || rect.bottom <= rect.top || total > static_cast<std::size_t>(width))
        return;
    const auto count = static_cast<int>(total);
    const auto gap = std::min(px(4, dpi), width / count - 1);
    const auto span = width + gap;
    const auto outline = highContrast ? GetSysColor(COLOR_WINDOWTEXT) : edge;
    for (std::size_t i = 0; i < total; ++i) {
        const auto index = static_cast<int>(i);
        RECT part{rect.left + MulDiv(index, span, count), rect.top,
                  rect.left + MulDiv(index + 1, span, count) - gap, rect.bottom};
        if (i < completed) {
            fill(dc, part, highContrast ? outline : cyan);
        } else {
            fill(dc, part, i == completed ? (highContrast ? GetSysColor(COLOR_HIGHLIGHT) : yellow) : outline);
            const LONG inset = i == completed ? px(2, dpi) : px(1, dpi);
            InflateRect(&part, -inset, -inset);
            if (part.right > part.left && part.bottom > part.top)
                fill(dc, part, base);
        }
    }
}

} // namespace chrome_axis::ui
