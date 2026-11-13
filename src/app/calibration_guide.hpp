#pragma once

#include <windows.h>

#include <cstddef>

namespace chrome_axis::ui {

enum class CalibrationPose { wheelCenter, wheelLeft, wheelRight, throttleRest, throttlePressed, brakeRest, brakePressed };

void paintCalibrationPose(HDC dc, const RECT& rect, UINT dpi, CalibrationPose pose, float motion, HFONT font = nullptr);
void paintCalibrationProgress(HDC dc, const RECT& rect, UINT dpi, std::size_t completed, std::size_t total);

} // namespace chrome_axis::ui
