#include "chrome_axis/brake_dialog.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"
#include "ui_theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace chrome_axis {
namespace {

constexpr UINT_PTR previewTimer = 1;
constexpr unsigned int previewInterval = 50;

struct AxisResponseText {
    Text title;
    Text heading;
    Text curve;
    Text saturation;
    Text intro;
    Text noSignal;
    Text invalidTest;
};

constexpr AxisResponseText steeringText{Text::mainSteeringSettingsTitle, Text::mainSteeringSettingsHeading,
    Text::mainSteeringSensitivity, Text::mainSteeringTravel, Text::mainSteeringSettingsIntro,
    Text::mainSteeringNoSignal, Text::mainSteeringInvalidTest};
constexpr AxisResponseText throttleText{Text::mainThrottleSettingsTitle, Text::mainThrottleSettingsHeading,
    Text::mainBrakeProgressivity, Text::mainThrottleTravel, Text::mainThrottleSettingsIntro,
    Text::mainBrakeNoSignal, Text::mainBrakeInvalidTestInput};
constexpr AxisResponseText brakeText{Text::mainBrakeSettingsTitle, Text::mainBrakeSettingsHeading,
    Text::mainBrakeProgressivity, Text::mainBrakeSaturation, Text::mainBrakeSettingsIntro,
    Text::mainBrakeNoSignal, Text::mainBrakeInvalidTestInput};

std::optional<int> readInteger(HWND window, int control, int minimum, int maximum)
{
    wchar_t text[8]{};
    const auto edit = GetDlgItem(window, control);
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0 || length >= static_cast<int>(std::size(text)) ||
        !GetWindowTextW(edit, text, static_cast<int>(std::size(text))))
        return std::nullopt;
    wchar_t* end{};
    const long value = std::wcstol(text, &end, 10);
    if (end == text || *end != L'\0' || value < minimum || value > maximum)
        return std::nullopt;
    return static_cast<int>(value);
}

struct AxisResponseDialog {
    DirectInput& input;
    ResponseAxis axis;
    const AxisCalibration& calibration;
    AxisResponse initial;
    std::optional<AxisResponse> result;
    std::optional<AxisPreview> displayed;

    void initialise(HWND window);
    void assignResponse(HWND window, const AxisResponse& response);
    std::optional<AxisResponse> response(HWND window) const;
    void updatePreview(HWND window);
    void paintMeter(const DRAWITEMSTRUCT& item) const;

    bool steering() const { return axis == ResponseAxis::steering; }
    const AxisResponseText& text() const
    {
        if (steering())
            return steeringText;
        return axis == ResponseAxis::throttle ? throttleText : brakeText;
    }
};

void AxisResponseDialog::initialise(HWND window)
{
    const auto& labels = text();
    SetWindowTextW(window, tr(labels.title));
    SetDlgItemTextW(window, IDC_BRAKE_PROGRESSIVITY_LABEL, tr(labels.curve));
    SetDlgItemTextW(window, IDC_BRAKE_SATURATION_LABEL, tr(labels.saturation));
    SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(labels.intro));
    SetDlgItemTextW(window, IDC_BRAKE_RAW_LABEL, tr(Text::mainBrakeRawTravel));
    SetDlgItemTextW(window, IDC_BRAKE_ADJUSTED_LABEL, tr(Text::mainBrakeAdjustedOutput));
    SetDlgItemTextW(window, IDC_BRAKE_SIMULATION_LABEL, tr(Text::mainBrakeTestInput));
    SetDlgItemTextW(window, IDC_BRAKE_RAW_VALUE, L"—");
    SetDlgItemTextW(window, IDC_BRAKE_ADJUSTED_VALUE, L"—");
    SetDlgItemTextW(window, IDC_BRAKE_RESET, tr(Text::mainBrakeDefaults));
    SetDlgItemTextW(window, IDOK, tr(Text::mainBrakeApply));
    SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
    SendDlgItemMessageW(window, IDC_BRAKE_PROGRESSIVITY, EM_SETLIMITTEXT, 4, 0);
    SendDlgItemMessageW(window, IDC_BRAKE_SATURATION, EM_SETLIMITTEXT, 3, 0);
    SendDlgItemMessageW(window, IDC_BRAKE_SIMULATED_TRAVEL, EM_SETLIMITTEXT, steering() ? 4 : 3, 0);
    DialogTheme::attach(window, tr(labels.heading), IDC_BRAKE_STATUS, 0, {IDOK});
    assignResponse(window, initial);
    SetTimer(window, previewTimer, previewInterval, nullptr);
}

void AxisResponseDialog::assignResponse(HWND window, const AxisResponse& response)
{
    SetDlgItemInt(window, IDC_BRAKE_PROGRESSIVITY, static_cast<UINT>(response.progressivity), TRUE);
    SetDlgItemInt(window, IDC_BRAKE_SATURATION, static_cast<UINT>(response.saturation), FALSE);
    updatePreview(window);
}

std::optional<AxisResponse> AxisResponseDialog::response(HWND window) const
{
    const auto progressivity = readInteger(window, IDC_BRAKE_PROGRESSIVITY, minimumBrakeProgressivity,
                                          maximumBrakeProgressivity);
    const auto saturation = readInteger(window, IDC_BRAKE_SATURATION, minimumBrakeSaturation,
                                       maximumBrakeSaturation);
    if (!progressivity || !saturation)
        return std::nullopt;
    return AxisResponse{*progressivity, *saturation};
}

void AxisResponseDialog::updatePreview(HWND window)
{
    const auto settings = response(window);
    EnableWindow(GetDlgItem(window, IDOK), settings.has_value());
    std::optional<long> raw;
    auto status = text().noSignal;
    const bool simulated = GetWindowTextLengthW(GetDlgItem(window, IDC_BRAKE_SIMULATED_TRAVEL)) > 0;
    if (!settings) {
        status = Text::mainBrakeInvalidSettings;
    } else if (simulated) {
        if (const auto travel = readInteger(window, IDC_BRAKE_SIMULATED_TRAVEL, steering() ? -100 : 0, 100)) {
            const double rest = calibration.neutral;
            const double end = *travel < 0 ? calibration.negative : calibration.positive;
            raw = std::lround(std::lerp(rest, end, std::abs(*travel) / 100.0));
            status = Text::mainBrakeSimulatedSignal;
        } else {
            status = text().invalidTest;
        }
    } else if (GetForegroundWindow() == window && !IsIconic(GetWindow(window, GW_OWNER))) {
        input.beginPoll();
        const auto sample = input.sample(calibration.deviceId);
        if (sample && sample->axesPresent[calibration.axis]) {
            raw = sample->axes[calibration.axis];
            status = Text::mainBrakeLiveSignal;
        }
    }

    const auto next = raw && settings ? std::optional{previewAxisResponse(axis, calibration, *raw, *settings)} : std::nullopt;
    const auto rounded = [](float value) { return std::lround(value * 100.0f); };
    const auto setGauge = [&](int meter, int value, float AxisPreview::* channel) {
        if (next.has_value() != displayed.has_value() ||
            (next && rounded((*next).*channel) != rounded((*displayed).*channel))) {
            std::wstring label = L"—";
            if (next) {
                const auto percent = rounded((*next).*channel);
                label = (steering() && percent > 0 ? L"+" : L"") + std::to_wstring(percent) + tr(Text::mainPercentSuffix);
            }
            SetDlgItemTextW(window, value, label.c_str());
            InvalidateRect(GetDlgItem(window, meter), nullptr, FALSE);
        }
    };
    setGauge(IDC_BRAKE_RAW_METER, IDC_BRAKE_RAW_VALUE, &AxisPreview::raw);
    setGauge(IDC_BRAKE_ADJUSTED_METER, IDC_BRAKE_ADJUSTED_VALUE, &AxisPreview::adjusted);
    displayed = next;
    wchar_t current[80]{};
    GetDlgItemTextW(window, IDC_BRAKE_STATUS, current, static_cast<int>(std::size(current)));
    if (std::wstring_view(current) != tr(status))
        SetDlgItemTextW(window, IDC_BRAKE_STATUS, tr(status));
}

void AxisResponseDialog::paintMeter(const DRAWITEMSTRUCT& item) const
{
    const bool contrast = ui::isHighContrast();
    const auto rect = item.rcItem;
    ui::fill(item.hDC, rect, contrast ? GetSysColor(COLOR_BTNFACE) : ui::field);
    const LONG origin = steering() ? rect.left + (rect.right - rect.left) / 2 : rect.left;
    if (displayed) {
        const bool raw = item.CtlID == IDC_BRAKE_RAW_METER;
        const float fraction = raw ? displayed->raw : displayed->adjusted;
        const LONG span = fraction < 0.0f ? origin - rect.left : rect.right - origin;
        const LONG end = origin + std::lround(fraction * static_cast<float>(span));
        if (end != origin) {
            ui::fill(item.hDC, {std::min(origin, end), rect.top, std::max(origin, end), rect.bottom},
                     contrast ? GetSysColor(COLOR_HIGHLIGHT) : raw ? ui::yellow : ui::cyan);
        }
    }
    if (steering())
        ui::fill(item.hDC, {origin, rect.top, origin + 1, rect.bottom}, contrast ? GetSysColor(COLOR_WINDOWTEXT) : ui::secondary);
}

INT_PTR CALLBACK axisResponseDialogProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        reinterpret_cast<AxisResponseDialog*>(lp)->initialise(window);
        return TRUE;
    }
    const auto dialog = reinterpret_cast<AxisResponseDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog)
        return FALSE;
    if (message == WM_TIMER && wp == previewTimer) {
        dialog->updatePreview(window);
        return TRUE;
    }
    if (message == WM_DRAWITEM && (wp == IDC_BRAKE_RAW_METER || wp == IDC_BRAKE_ADJUSTED_METER)) {
        dialog->paintMeter(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    }
    if (message == WM_DESTROY) {
        KillTimer(window, previewTimer);
        return FALSE;
    }
    if (message != WM_COMMAND)
        return FALSE;
    switch (LOWORD(wp)) {
    case IDC_BRAKE_PROGRESSIVITY:
    case IDC_BRAKE_SATURATION:
    case IDC_BRAKE_SIMULATED_TRAVEL:
        if (HIWORD(wp) == EN_CHANGE)
            dialog->updatePreview(window);
        return TRUE;
    case IDC_BRAKE_RESET:
        dialog->assignResponse(window, {});
        return TRUE;
    case IDOK:
        if (dialog->result = dialog->response(window); dialog->result)
            EndDialog(window, IDOK);
        return TRUE;
    case IDCANCEL:
        EndDialog(window, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

std::optional<AxisResponse> editAxisResponse(HWND owner, DirectInput& input, ResponseAxis axis,
    const AxisCalibration& calibration, const AxisResponse& response)
{
    AxisResponseDialog dialog{input, axis, calibration, response, {}, {}};
    const auto code = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_BRAKE_SETTINGS), owner,
                                      axisResponseDialogProc, reinterpret_cast<LPARAM>(&dialog));
    return code == IDOK ? dialog.result : std::nullopt;
}

} // namespace chrome_axis
