#include "calibration_guide.hpp"

#include "ui_theme.hpp"

#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>
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

bool paintProduct(HDC dc, const Diagram& diagram, CalibrationPose pose)
{
    const bool pedal = pose == CalibrationPose::pedalRest || pose == CalibrationPose::pedalPressed;
    static const CalibrationArtwork artwork;
    auto image = artwork.image(pedal);
    if (!image)
        return false;

    const int turn = pose == CalibrationPose::wheelLeft ? -1 : pose == CalibrationPose::wheelRight ? 1 : 0;
    const bool pressed = pose == CalibrationPose::pedalPressed;
    const auto pivot = pedal ? diagram.point(132, 111) : diagram.point(100, 76);
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // GDI+ uses pixel coordinates. Match the viewport origin of the Win32 paint buffer.
        POINT origin{};
        GetViewportOrgEx(dc, &origin);
        graphics.TranslateTransform(static_cast<float>(pivot.x + origin.x), static_cast<float>(pivot.y + origin.y));
        // Use the same artwork in each pose, rotating about its hub or hinge.
        graphics.RotateTransform(pedal ? (pressed ? -22.0f : 0.0f) : static_cast<float>(turn * 42));
        const int width = diagram.scale(pedal ? 112 : 118);
        const int height = MulDiv(width, static_cast<int>(image->GetHeight()), static_cast<int>(image->GetWidth()));
        const int left = pedal ? -MulDiv(width, 91, 100) : -width / 2;
        const int top = pedal ? -MulDiv(height, 88, 100) : -height / 2;
        if (graphics.DrawImage(image, left, top, width, height) != Gdiplus::Ok)
            return false;
    }

    const auto motion = CreatePen(PS_SOLID, std::max(1L, diagram.scale(3)), cyan);
    if (!motion)
        return true;
    const auto previous = SelectObject(dc, motion);
    if (pedal) {
        diagram.line(dc, {{33, 124}, {147, 124}});
        const LONG from = pressed ? 43 : 87;
        const LONG to = pressed ? 87 : 43;
        const LONG tip = to + (pressed ? -8 : 8);
        diagram.line(dc, {{165, from}, {165, to}});
        diagram.line(dc, {{156, tip}, {165, to}, {174, tip}});
    } else if (turn) {
        const auto side = [turn](LONG offset) { return 100 + turn * offset; };
        diagram.line(dc, {{side(14), 10}, {side(39), 19}, {side(56), 35}, {side(63), 57}});
        diagram.line(dc, {{side(52), 51}, {side(63), 57}, {side(68), 46}});
    } else {
        diagram.line(dc, {{94, 8}, {100, 14}, {106, 8}});
    }
    SelectObject(dc, previous);
    DeleteObject(motion);
    return true;
}

void paintWheel(HDC dc, const Diagram& diagram, CalibrationPose pose, HPEN outline, HPEN mark, HPEN motion)
{
    const int turn = pose == CalibrationPose::wheelLeft ? -1 : pose == CalibrationPose::wheelRight ? 1 : 0;
    const auto wheelPoint = [&](LONG x, LONG y) {
        if (turn != 0)
            return diagram.point(100 + MulDiv(x - turn * y, 707, 1000), 75 + MulDiv(turn * x + y, 707, 1000));
        return diagram.point(100 + x, 75 + y);
    };
    SelectObject(dc, outline);
    diagram.ellipse(dc, 100, 75, 36, 36);
    diagram.ellipse(dc, 100, 75, 6, 6);
    const POINT leftSpoke[]{wheelPoint(-31, -12), wheelPoint(-6, -2)};
    const POINT rightSpoke[]{wheelPoint(31, -12), wheelPoint(6, -2)};
    const POINT lowerSpoke[]{wheelPoint(0, 7), wheelPoint(0, 34)};
    Polyline(dc, leftSpoke, 2);
    Polyline(dc, rightSpoke, 2);
    Polyline(dc, lowerSpoke, 2);

    SelectObject(dc, mark);
    const POINT stripe[]{wheelPoint(0, -34), wheelPoint(0, -43)};
    Polyline(dc, stripe, 2);

    SelectObject(dc, motion);
    if (turn == 0) {
        diagram.line(dc, {{94, 21}, {100, 27}, {106, 21}});
        return;
    }
    const auto side = [turn](LONG offset) { return 100 + turn * offset; };
    diagram.line(dc, {{side(4), 23}, {side(21), 27}, {side(37), 38}, {side(47), 53}});
    diagram.line(dc, {{side(37), 50}, {side(47), 53}, {side(49), 43}});
}

void paintPedal(HDC dc, const Diagram& diagram, bool pressed, HPEN outline, HPEN mark, HPEN motion)
{
    SelectObject(dc, outline);
    diagram.line(dc, {{42, 115}, {160, 115}});
    diagram.line(dc, {{100, 115}, {113, 102}, {126, 115}});
    diagram.ellipse(dc, 113, 105, 3, 3);
    if (pressed)
        diagram.line(dc, {{112, 102}, {81, 84}});
    else
        diagram.line(dc, {{112, 102}, {81, 64}});

    SelectObject(dc, mark);
    if (pressed)
        diagram.line(dc, {{53, 79}, {58, 69}, {100, 80}, {96, 90}, {53, 79}});
    else
        diagram.line(dc, {{54, 47}, {65, 39}, {96, 66}, {86, 74}, {54, 47}});

    SelectObject(dc, motion);
    if (pressed) {
        diagram.line(dc, {{135, 42}, {135, 83}});
        diagram.line(dc, {{126, 74}, {135, 83}, {144, 74}});
    } else {
        diagram.line(dc, {{135, 83}, {135, 42}});
        diagram.line(dc, {{126, 51}, {135, 42}, {144, 51}});
    }
}

} // namespace

void paintCalibrationPose(HDC dc, const RECT& rect, UINT dpi, CalibrationPose pose)
{
    const auto highContrast = isHighContrast();
    const int width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top;
    const auto scaleWidth = std::min({width, MulDiv(height, diagramWidth, diagramHeight), px(diagramWidth, dpi)});
    if (scaleWidth < 24)
        return;

    const auto scaleHeight = MulDiv(scaleWidth, diagramHeight, diagramWidth);
    const Diagram diagram{rect.left + (width - scaleWidth) / 2, rect.top + (height - scaleHeight) / 2, scaleWidth};
    const auto textColor = highContrast ? GetSysColor(COLOR_WINDOWTEXT) : secondary;
    fill(dc, rect, highContrast ? GetSysColor(COLOR_WINDOW) : panel);
    cutPanel(dc, {diagram.left, diagram.top, diagram.left + scaleWidth, diagram.top + scaleHeight},
             diagram.scale(12), highContrast ? GetSysColor(COLOR_WINDOW) : field,
             highContrast ? textColor : edge);

    if (!highContrast && paintProduct(dc, diagram, pose))
        return;

    const auto saved = SaveDC(dc);
    if (!saved)
        return;
    const auto outline = CreatePen(PS_SOLID, std::max(1L, diagram.scale(3)), textColor);
    const auto mark = CreatePen(PS_SOLID, std::max(1L, diagram.scale(4)), highContrast ? textColor : yellow);
    const auto motion = CreatePen(PS_SOLID, std::max(1L, diagram.scale(3)), highContrast ? textColor : cyan);
    if (outline && mark && motion) {
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (pose == CalibrationPose::pedalRest || pose == CalibrationPose::pedalPressed)
            paintPedal(dc, diagram, pose == CalibrationPose::pedalPressed, outline, mark, motion);
        else
            paintWheel(dc, diagram, pose, outline, mark, motion);
    }
    RestoreDC(dc, saved);
    DeleteObject(motion);
    DeleteObject(mark);
    DeleteObject(outline);
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
