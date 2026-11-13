#pragma once

#include <windows.h>

#include <cstddef>

namespace chrome_axis::ui {

enum class CalibrationPose { wheelCenter, wheelLeft, wheelRight, pedalRest, pedalPressed };

void paintCalibrationPose(HDC dc, const RECT& rect, UINT dpi, CalibrationPose pose);
void paintCalibrationProgress(HDC dc, const RECT& rect, UINT dpi, std::size_t completed, std::size_t total);

} // namespace chrome_axis::ui
