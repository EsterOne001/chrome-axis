#include "chrome_axis/brake_dialog.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"
#include "ui_theme.hpp"

#include <cmath>
#include <cstdlib>
#include <iterator>

namespace chrome_axis {
namespace {

constexpr UINT_PTR previewTimer = 1;
constexpr unsigned int previewInterval = 50;

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

struct BrakeDialog {
    DirectInput& input;
    const AxisCalibration& calibration;
    BrakeResponse initial;
    std::optional<BrakeResponse> result;
    std::optional<BrakePreview> displayed;

    void initialise(HWND window);
    void assignResponse(HWND window, const BrakeResponse& response);
    std::optional<BrakeResponse> response(HWND window) const;
    void updatePreview(HWND window);
    void paintMeter(const DRAWITEMSTRUCT& item) const;
};

void BrakeDialog::initialise(HWND window)
{
    SetWindowTextW(window, tr(Text::mainBrakeSettingsTitle));
    SetDlgItemTextW(window, IDC_BRAKE_PROGRESSIVITY_LABEL, tr(Text::mainBrakeProgressivity));
    SetDlgItemTextW(window, IDC_BRAKE_SATURATION_LABEL, tr(Text::mainBrakeSaturation));
    SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(Text::mainBrakeSettingsIntro));
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
    SendDlgItemMessageW(window, IDC_BRAKE_SIMULATED_TRAVEL, EM_SETLIMITTEXT, 3, 0);
    DialogTheme::attach(window, tr(Text::mainBrakeSettingsHeading), IDC_BRAKE_STATUS, 0, {IDOK});
    assignResponse(window, initial);
    SetTimer(window, previewTimer, previewInterval, nullptr);
}

void BrakeDialog::assignResponse(HWND window, const BrakeResponse& response)
{
    SetDlgItemInt(window, IDC_BRAKE_PROGRESSIVITY, static_cast<UINT>(response.progressivity), TRUE);
    SetDlgItemInt(window, IDC_BRAKE_SATURATION, static_cast<UINT>(response.saturation), FALSE);
    updatePreview(window);
}

std::optional<BrakeResponse> BrakeDialog::response(HWND window) const
{
    const auto progressivity = readInteger(window, IDC_BRAKE_PROGRESSIVITY, minimumBrakeProgressivity,
                                          maximumBrakeProgressivity);
    const auto saturation = readInteger(window, IDC_BRAKE_SATURATION, minimumBrakeSaturation,
                                       maximumBrakeSaturation);
    if (!progressivity || !saturation)
        return std::nullopt;
    return BrakeResponse{*progressivity, *saturation};
}

void BrakeDialog::updatePreview(HWND window)
{
    const auto settings = response(window);
    EnableWindow(GetDlgItem(window, IDOK), settings.has_value());
    std::optional<long> raw;
    auto status = Text::mainBrakeNoSignal;
    const bool simulated = GetWindowTextLengthW(GetDlgItem(window, IDC_BRAKE_SIMULATED_TRAVEL)) > 0;
    if (!settings) {
        status = Text::mainBrakeInvalidSettings;
    } else if (simulated) {
        if (const auto travel = readInteger(window, IDC_BRAKE_SIMULATED_TRAVEL, 0, 100)) {
            const double rest = calibration.neutral;
            const double span = static_cast<double>(calibration.positive) - rest;
            raw = std::lround(rest + span * *travel / 100.0);
            status = Text::mainBrakeSimulatedSignal;
        } else {
            status = Text::mainBrakeInvalidTestInput;
        }
    } else if (GetForegroundWindow() == window && !IsIconic(GetWindow(window, GW_OWNER))) {
        input.beginPoll();
        const auto sample = input.sample(calibration.deviceId);
        if (sample && sample->axesPresent[calibration.axis]) {
            raw = sample->axes[calibration.axis];
            status = Text::mainBrakeLiveSignal;
        }
    }

    const auto next = raw && settings ? std::optional{previewBrake(calibration, *raw, *settings)} : std::nullopt;
    const auto rounded = [](float value) { return std::lround(value * 100.0f); };
    const auto setGauge = [&](int meter, int value, float BrakePreview::* channel) {
        if (next.has_value() != displayed.has_value() ||
            (next && rounded((*next).*channel) != rounded((*displayed).*channel))) {
            const auto label = next ? std::to_wstring(rounded((*next).*channel)) + tr(Text::mainPercentSuffix) : L"—";
            SetDlgItemTextW(window, value, label.c_str());
            InvalidateRect(GetDlgItem(window, meter), nullptr, FALSE);
        }
    };
    setGauge(IDC_BRAKE_RAW_METER, IDC_BRAKE_RAW_VALUE, &BrakePreview::raw);
    setGauge(IDC_BRAKE_ADJUSTED_METER, IDC_BRAKE_ADJUSTED_VALUE, &BrakePreview::adjusted);
    displayed = next;
    wchar_t current[80]{};
    GetDlgItemTextW(window, IDC_BRAKE_STATUS, current, static_cast<int>(std::size(current)));
    if (std::wstring_view(current) != tr(status))
        SetDlgItemTextW(window, IDC_BRAKE_STATUS, tr(status));
}

void BrakeDialog::paintMeter(const DRAWITEMSTRUCT& item) const
{
    const bool contrast = ui::isHighContrast();
    auto rect = item.rcItem;
    ui::fill(item.hDC, rect, contrast ? GetSysColor(COLOR_BTNFACE) : ui::field);
    if (!displayed)
        return;
    const bool raw = item.CtlID == IDC_BRAKE_RAW_METER;
    const float fraction = raw ? displayed->raw : displayed->adjusted;
    rect.right = rect.left + std::lround(fraction * static_cast<float>(rect.right - rect.left));
    if (rect.right > rect.left)
        ui::fill(item.hDC, rect, contrast ? GetSysColor(COLOR_HIGHLIGHT) : raw ? ui::yellow : ui::cyan);
}

INT_PTR CALLBACK brakeDialogProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        reinterpret_cast<BrakeDialog*>(lp)->initialise(window);
        return TRUE;
    }
    const auto dialog = reinterpret_cast<BrakeDialog*>(GetWindowLongPtrW(window, DWLP_USER));
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

std::optional<BrakeResponse> editBrakeResponse(HWND owner, DirectInput& input,
    const AxisCalibration& calibration, const BrakeResponse& response)
{
    BrakeDialog dialog{input, calibration, response, {}, {}};
    const auto code = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_BRAKE_SETTINGS), owner,
                                      brakeDialogProc, reinterpret_cast<LPARAM>(&dialog));
    return code == IDOK ? dialog.result : std::nullopt;
}

} // namespace chrome_axis
