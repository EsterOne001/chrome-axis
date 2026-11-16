#include "chrome_axis/brake_dialog.hpp"
#include "app_input.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"
#include "ui_theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
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

constexpr std::uint64_t chartDuration = 8000;
constexpr std::uint64_t maximumGameSampleAge = 500;
constexpr std::array pedalAxes{ResponseAxis::brake, ResponseAxis::throttle};
constexpr std::array pedalTestIds{IDC_PEDAL_BRAKE_TEST, IDC_PEDAL_THROTTLE_TEST};
constexpr std::array pedalGraphIds{IDC_PEDAL_BRAKE_GRAPH, IDC_PEDAL_THROTTLE_GRAPH};
constexpr std::array pedalKeyIds{IDC_PEDAL_KEY_RAW, IDC_PEDAL_KEY_CORRECTED, IDC_PEDAL_KEY_GAME};
constexpr int pedalValueIds[2][3]{
    {IDC_PEDAL_BRAKE_RAW, IDC_PEDAL_BRAKE_CORRECTED, IDC_PEDAL_BRAKE_GAME},
    {IDC_PEDAL_THROTTLE_RAW, IDC_PEDAL_THROTTLE_CORRECTED, IDC_PEDAL_THROTTLE_GAME},
};
constexpr std::array pedalColors{ui::yellow, ui::cyan, ui::ink};
constexpr std::array pedalLineStyles{PS_DOT, PS_DASH, PS_SOLID};

using PedalValues = std::array<std::optional<float>, 3>;

struct PedalFrame {
    std::uint64_t at{};
    std::array<PedalValues, 2> pedals{};
};

bool hasPedalValues(const PedalFrame& frame)
{
    for (const auto& pedal : frame.pedals)
        for (const auto value : pedal)
            if (value)
                return true;
    return false;
}

void appendFrame(std::deque<PedalFrame>& history, PedalFrame frame)
{
    if (!history.empty() && frame.at - history.back().at < previewInterval / 2)
        history.back() = frame;
    else
        history.push_back(frame);
    while (history.size() > 1 && frame.at - history[1].at > chartDuration)
        history.pop_front();
}

void setChangedLabel(HWND window, int id, const std::wstring_view next)
{
    wchar_t current[100]{};
    GetDlgItemTextW(window, id, current, static_cast<int>(std::size(current)));
    if (current != next)
        SetDlgItemTextW(window, id, next.data());
}

struct AxisResponseDialog {
    AppInput& input;
    ResponseAxis axis;
    const AxisCalibration* calibration;
    AxisResponse initial;
    const PedalGraphContext* pedals;
    std::optional<AxisResponse> result;
    std::optional<AxisPreview> displayed;
    std::deque<PedalFrame> localHistory;
    std::deque<PedalFrame> gameHistory;
    std::uint64_t chartEnd{};
    bool showingGame{};
    bool showPreviousGame{};
    bool simulatingPedals{};
    std::array<PedalStabilizer, 2> pedalStabilizers{};

    void initialise(HWND window);
    void assignResponse(HWND window, const AxisResponse& response);
    std::optional<AxisResponse> response(HWND window) const;
    void updatePreview(HWND window);
    void initialisePedals(HWND window);
    void updatePedals(HWND window, const std::optional<AxisResponse>& settings);
    void applyBrakePreset(HWND window);
    void paintPedals(const DRAWITEMSTRUCT& item) const;
    std::optional<PedalFrame> currentGamePedals(std::uint64_t now) const;
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
    SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(axis == ResponseAxis::brake && pedals ? Text::DlgPedalPresetNote : labels.intro));
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
    if (pedals)
        initialisePedals(window);
    DialogTheme::attach(window, tr(labels.heading), IDC_BRAKE_STATUS, 0, {IDOK});
    assignResponse(window, initial);
    SetTimer(window, previewTimer, previewInterval, nullptr);
}

void AxisResponseDialog::assignResponse(HWND window, const AxisResponse& response)
{
    if (pedals)
        CheckDlgButton(window, IDC_PEDAL_STABILIZATION, response.stabilization ? BST_CHECKED : BST_UNCHECKED);
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
    return AxisResponse{*progressivity, *saturation,
                        pedals && IsDlgButtonChecked(window, IDC_PEDAL_STABILIZATION) == BST_CHECKED};
}

void AxisResponseDialog::updatePreview(HWND window)
{
    const auto settings = response(window);
    EnableWindow(GetDlgItem(window, IDOK), settings.has_value());
    if (pedals) {
        updatePedals(window, settings);
        return;
    }
    if (!calibration)
        return;

    std::optional<long> raw;
    auto status = text().noSignal;
    const bool simulated = GetWindowTextLengthW(GetDlgItem(window, IDC_BRAKE_SIMULATED_TRAVEL)) > 0;
    if (!settings) {
        status = Text::mainBrakeInvalidSettings;
    } else if (simulated) {
        if (const auto travel = readInteger(window, IDC_BRAKE_SIMULATED_TRAVEL, steering() ? -100 : 0, 100)) {
            const double rest = calibration->neutral;
            const double end = *travel < 0 ? calibration->negative : calibration->positive;
            raw = std::lround(std::lerp(rest, end, std::abs(*travel) / 100.0));
            status = Text::mainBrakeSimulatedSignal;
        } else {
            status = text().invalidTest;
        }
    } else if (GetForegroundWindow() == window && !IsIconic(GetWindow(window, GW_OWNER))) {
        const auto sample = input.sample(calibration->deviceId);
        if (sample && sample->axesPresent[calibration->axis]) {
            raw = sample->axes[calibration->axis];
            status = Text::mainBrakeLiveSignal;
        }
    }

    const auto next = raw && settings ? std::optional{previewAxisResponse(axis, *calibration, *raw, *settings)} : std::nullopt;
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
    setChangedLabel(window, IDC_BRAKE_STATUS, tr(status));
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

void AxisResponseDialog::initialisePedals(HWND window)
{
    constexpr std::pair<int, Text> labels[]{
        {IDC_MODAL_LIST_HEADING, Text::DlgPedalChartHeading},
        {IDC_PEDAL_RAW_LABEL, Text::DlgPedalChartRaw},
        {IDC_PEDAL_CORRECTED_LABEL, Text::DlgPedalChartCorrected},
        {IDC_PEDAL_GAME_LABEL, Text::DlgPedalChartGame},
        {IDC_PEDAL_BRAKE_LABEL, Text::mainBrake},
        {IDC_PEDAL_THROTTLE_LABEL, Text::mainThrottle},
        {IDC_PEDAL_BRAKE_TEST_LABEL, Text::DlgPedalTestBrake},
        {IDC_PEDAL_THROTTLE_TEST_LABEL, Text::DlgPedalTestThrottle},
        {IDC_PEDAL_TIME_START, Text::DlgPedalChartStart},
        {IDC_PEDAL_HISTORY, Text::DlgPedalGameHistory},
        {IDC_PEDAL_STABILIZATION, Text::DlgPedalStabilization},
    };
    for (const auto& [id, label] : labels)
        SetDlgItemTextW(window, id, tr(label));
    for (const auto id : pedalTestIds)
        SendDlgItemMessageW(window, id, EM_SETLIMITTEXT, 3, 0);
    EnableWindow(GetDlgItem(window, IDC_PEDAL_HISTORY), FALSE);

    if (axis != ResponseAxis::brake) {
        ShowWindow(GetDlgItem(window, IDC_PEDAL_PRESET_LABEL), SW_HIDE);
        ShowWindow(GetDlgItem(window, IDC_PEDAL_PRESET), SW_HIDE);
        return;
    }
    SetDlgItemTextW(window, IDC_PEDAL_PRESET_LABEL, tr(Text::DlgPedalPresetLabel));
    for (const auto label : {Text::DlgPedalPresetCustom, Text::DlgPedalPresetPosition, Text::DlgPedalPresetPressure}) {
        SendDlgItemMessageW(window, IDC_PEDAL_PRESET, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(label)));
    }
    SendDlgItemMessageW(window, IDC_PEDAL_PRESET, CB_SETCURSEL, 0, 0);
}

constexpr std::array presetProgressivity{0, 50, 0}; // Custom, travel pedal, load cell.

void AxisResponseDialog::applyBrakePreset(HWND window)
{
    const auto selected = SendDlgItemMessageW(window, IDC_PEDAL_PRESET, CB_GETCURSEL, 0, 0);
    if (selected > 0 && selected < static_cast<LRESULT>(presetProgressivity.size())) {
        SetDlgItemInt(window, IDC_BRAKE_PROGRESSIVITY, static_cast<UINT>(presetProgressivity[selected]), TRUE);
    }
}

std::optional<PedalFrame> AxisResponseDialog::currentGamePedals(std::uint64_t now) const
{
    const auto report = pedals->diagnostics.read();
    const auto& snap = report.snapshot;
    if (report.connection != DiagnosticConnection::connected || snap.mode != DiagnosticMode::usb ||
        !snap.profileLoaded || !snap.pedalSampledAt || snap.pedalSampledAt > now ||
        now - snap.pedalSampledAt > maximumGameSampleAge)
        return std::nullopt;

    const auto end = std::ranges::find(snap.profile, '\0');
    const std::string_view profile(snap.profile.begin(), end);
    const std::string_view selected(reinterpret_cast<const char*>(pedals->profileName.data()), pedals->profileName.size());
    if (profile != selected)
        return std::nullopt;

    PedalFrame frame{snap.pedalSampledAt};
    for (std::size_t pedal = 0; pedal < pedalAxes.size(); ++pedal) {
        const auto& delivered = snap.axes[pedal == 0 ? 2 : 1];
        const float values[]{delivered.inputValue, delivered.requestedValue, delivered.finalValue};
        const auto available = {delivered.inputAvailable, delivered.requestedAvailable, delivered.finalAvailable};
        std::size_t series = 0;
        for (const bool valid : available) {
            if (valid && std::isfinite(values[series]))
                frame.pedals[pedal][series] = std::clamp(values[series], 0.0f, 1.0f);
            ++series;
        }
    }
    return hasPedalValues(frame) ? std::optional{frame} : std::nullopt;
}

void AxisResponseDialog::updatePedals(HWND window, const std::optional<AxisResponse>& settings)
{
    if (axis == ResponseAxis::brake) {
        const auto selected = SendDlgItemMessageW(window, IDC_PEDAL_PRESET, CB_GETCURSEL, 0, 0);
        if (selected > 0 && selected < static_cast<LRESULT>(presetProgressivity.size()) &&
            readInteger(window, IDC_BRAKE_PROGRESSIVITY, minimumBrakeProgressivity, maximumBrakeProgressivity)
                != presetProgressivity[selected]) {
            SendDlgItemMessageW(window, IDC_PEDAL_PRESET, CB_SETCURSEL, 0, 0);
        }
    }

    const auto now = GetTickCount64();
    const auto gameFrame = currentGamePedals(now);
    if (gameFrame && (gameHistory.empty() || gameFrame->at > gameHistory.back().at))
        appendFrame(gameHistory, *gameFrame);

    const auto hasTest = [&](int id) { return GetWindowTextLengthW(GetDlgItem(window, id)) > 0; };
    const bool simulated = std::ranges::any_of(pedalTestIds, hasTest);
    if (simulated != simulatingPedals) {
        localHistory.clear();
        for (auto& stabilizer : pedalStabilizers)
            stabilizer.reset();
        simulatingPedals = simulated;
    }

    PedalFrame local{now};
    bool invalidTest = false;
    const bool canRead = !simulated && GetForegroundWindow() == window && !IsIconic(GetWindow(window, GW_OWNER));
    const auto nowSampled = PedalStabilizer::Clock::now();
    const AxisCalibration simulatedCalibration{.deviceId = {}, .positive = 100};
    for (std::size_t pedal = 0; pedal < pedalAxes.size(); ++pedal) {
        const bool editing = pedalAxes[pedal] == axis;
        const auto response = editing ? settings : std::optional{pedals->otherResponse};
        const auto* binding = editing ? calibration : pedals->otherCalibration;
        std::optional<long> raw;
        auto sampledAt = nowSampled;
        if (simulated) {
            const auto id = pedalTestIds[pedal];
            if (hasTest(id)) {
                const auto travel = readInteger(window, id, 0, 100);
                if (travel) {
                    if (!binding)
                        binding = &simulatedCalibration;
                    raw = std::lround(std::lerp(static_cast<double>(binding->neutral),
                        static_cast<double>(binding->positive), *travel / 100.0));
                } else {
                    invalidTest = true;
                }
            }
        } else if (canRead && binding) {
            if (const auto sample = input.sample(binding->deviceId); sample && sample->axesPresent[binding->axis]) {
                raw = sample->axes[binding->axis];
                sampledAt = sample->sampledAt;
            }
        }
        if (raw && response) {
            const auto preview = previewAxisResponse(pedalAxes[pedal], *binding, *raw, *response);
            const auto adjusted = response->stabilization
                ? pedalStabilizers[pedal].update(preview.adjusted, sampledAt) : preview.adjusted;
            local.pedals[pedal] = {preview.raw, adjusted, std::nullopt};
            if (response->stabilization)
                continue;
        }
        pedalStabilizers[pedal].reset();
    }

    if (hasPedalValues(local) || !localHistory.empty())
        appendFrame(localHistory, local);
    const bool historyAvailable = !gameHistory.empty();
    EnableWindow(GetDlgItem(window, IDC_PEDAL_HISTORY), historyAvailable && !simulated);
    showingGame = !simulated && (gameFrame.has_value() || (historyAvailable && showPreviousGame));
    const bool frozen = showingGame && !gameFrame;
    const auto& values = showingGame ? gameHistory.back() : local;
    chartEnd = frozen ? values.at : now;

    for (std::size_t pedal = 0; pedal < pedalAxes.size(); ++pedal) {
        for (std::size_t series = 0; series < values.pedals[pedal].size(); ++series) {
            const auto value = values.pedals[pedal][series];
            const std::wstring label = value ? std::to_wstring(std::lround(*value * 100.0f))
                + tr(Text::mainPercentSuffix) : L"—";
            setChangedLabel(window, pedalValueIds[pedal][series], label);
        }
        InvalidateRect(GetDlgItem(window, pedalGraphIds[pedal]), nullptr, FALSE);
    }

    const auto status = frozen ? Text::DlgPedalArchivedGame
        : showingGame ? Text::DlgPedalLiveGame
        : !settings ? Text::mainBrakeInvalidSettings
        : invalidTest ? Text::mainBrakeInvalidTestInput
        : simulated ? Text::DlgPedalSimulated
        : hasPedalValues(local) ? Text::DlgPedalLocal
        : Text::DlgPedalWait;
    setChangedLabel(window, IDC_BRAKE_STATUS, tr(status));
    setChangedLabel(window, IDC_PEDAL_TIME_END, tr(frozen ? Text::DlgPedalChartStop : Text::DlgPedalChartNow));
    setChangedLabel(window, IDC_PEDAL_HISTORY, tr(showPreviousGame ? Text::DlgPedalBackToPreview : Text::DlgPedalGameHistory));
}

void AxisResponseDialog::paintPedals(const DRAWITEMSTRUCT& item) const
{
    const bool contrast = ui::isHighContrast();
    const auto rect = item.rcItem;
    const auto dc = item.hDC;
    const auto textColor = GetSysColor(COLOR_WINDOWTEXT);
    const auto background = contrast ? GetSysColor(COLOR_WINDOW) : ui::field;
    const auto key = std::ranges::find(pedalKeyIds, static_cast<int>(item.CtlID));
    if (key != pedalKeyIds.end()) {
        const auto series = static_cast<std::size_t>(key - pedalKeyIds.begin());
        ui::fill(dc, rect, contrast ? background : ui::panel);
        const auto pen = CreatePen(pedalLineStyles[series], series == 2 ? 2 : 1, contrast ? textColor : pedalColors[series]);
        const auto previous = SelectObject(dc, pen);
        const LONG center = (rect.top + rect.bottom) / 2;
        const auto mode = SetBkMode(dc, TRANSPARENT);
        MoveToEx(dc, rect.left, center, nullptr);
        LineTo(dc, rect.right, center);
        SetBkMode(dc, mode);
        SelectObject(dc, previous);
        DeleteObject(pen);
        return;
    }

    const LONG width = rect.right - rect.left - 1;
    const LONG height = rect.bottom - rect.top - 1;
    if (width <= 0 || height <= 0)
        return;
    ui::fill(dc, rect, background);
    const auto grid = contrast ? GetSysColor(COLOR_GRAYTEXT) : ui::edge;
    for (LONG tick = 0; tick <= 8; ++tick) {
        const LONG x = rect.left + width * tick / 8;
        ui::fill(dc, {x, rect.top, x + 1, rect.bottom}, grid);
    }
    for (LONG mark = 0; mark <= 4; ++mark) {
        const LONG y = rect.top + height * mark / 4;
        ui::fill(dc, {rect.left, y, rect.right, y + 1}, grid);
    }

    const std::size_t pedal = item.CtlID == IDC_PEDAL_BRAKE_GRAPH ? 0 : 1;
    const auto& history = showingGame ? gameHistory : localHistory;
    const auto saved = SaveDC(dc);
    if (!saved)
        return;
    IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    SetBkMode(dc, TRANSPARENT);
    for (std::size_t series = 0; series < pedalColors.size(); ++series) {
        const auto color = contrast ? textColor : pedalColors[series];
        const auto pen = CreatePen(pedalLineStyles[series], series == 2 ? 2 : 1, color);
        const auto previous = SelectObject(dc, pen);
        std::uint64_t previousAt{};
        for (const auto& frame : history) {
            const auto value = frame.pedals[pedal][series];
            if (!value || frame.at > chartEnd || chartEnd - frame.at > chartDuration + maximumGameSampleAge) {
                previousAt = 0;
                continue;
            }
            const LONG x = rect.right - 1 - std::lround(static_cast<double>(chartEnd - frame.at) * width / chartDuration);
            const LONG y = rect.bottom - 1 - std::lround(*value * static_cast<float>(height));
            if (previousAt && frame.at - previousAt <= (showingGame ? maximumGameSampleAge : 3 * previewInterval))
                LineTo(dc, x, y);
            else {
                MoveToEx(dc, x, y, nullptr);
                SetPixelV(dc, x, y, color);
            }
            previousAt = frame.at;
        }
        SelectObject(dc, previous);
        DeleteObject(pen);
    }
    RestoreDC(dc, saved);
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
    if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (wp == IDC_BRAKE_RAW_METER || wp == IDC_BRAKE_ADJUSTED_METER) {
            dialog->paintMeter(item);
            return TRUE;
        }
        if (dialog->pedals && (wp == IDC_PEDAL_BRAKE_GRAPH || wp == IDC_PEDAL_THROTTLE_GRAPH ||
            (wp >= IDC_PEDAL_KEY_RAW && wp <= IDC_PEDAL_KEY_GAME))) {
            dialog->paintPedals(item);
            return TRUE;
        }
    }
    if (message == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
        dialog->input.invalidate();
        return FALSE;
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
    case IDC_PEDAL_BRAKE_TEST:
    case IDC_PEDAL_THROTTLE_TEST:
        if (HIWORD(wp) == EN_CHANGE)
            dialog->updatePreview(window);
        return TRUE;
    case IDC_PEDAL_PRESET:
        if (HIWORD(wp) == CBN_SELCHANGE)
            dialog->applyBrakePreset(window);
        return TRUE;
    case IDC_PEDAL_STABILIZATION:
        if (HIWORD(wp) == BN_CLICKED)
            dialog->updatePreview(window);
        return TRUE;
    case IDC_PEDAL_HISTORY:
        if (HIWORD(wp) == BN_CLICKED) {
            dialog->showPreviousGame = !dialog->showPreviousGame;
            dialog->updatePreview(window);
        }
        return TRUE;
    case IDC_BRAKE_RESET:
        if (dialog->pedals && dialog->axis == ResponseAxis::brake)
            SendDlgItemMessageW(window, IDC_PEDAL_PRESET, CB_SETCURSEL, 0, 0);
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

std::optional<AxisResponse> editAxisResponse(HWND owner, AppInput& input, ResponseAxis axis,
    const AxisCalibration* calibration, const AxisResponse& response, const PedalGraphContext* pedals)
{
    AxisResponseDialog dialog{
        .input = input, .axis = axis, .calibration = calibration, .initial = response, .pedals = pedals,
        .result = {}, .displayed = {}, .localHistory = {}, .gameHistory = {},
    };
    const auto resource = MAKEINTRESOURCEW(pedals ? IDD_PEDAL_SETTINGS : IDD_BRAKE_SETTINGS);
    const auto code = DialogBoxParamW(GetModuleHandleW(nullptr), resource, owner,
                                      axisResponseDialogProc, reinterpret_cast<LPARAM>(&dialog));
    return code == IDOK ? dialog.result : std::nullopt;
}

} // namespace chrome_axis
