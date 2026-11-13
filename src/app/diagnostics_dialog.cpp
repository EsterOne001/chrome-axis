#include "diagnostics_dialog.hpp"

#include "app_text.hpp"
#include "chrome_axis/accessories.hpp"
#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "dialog_theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace chrome_axis {
namespace {

constexpr UINT_PTR refreshTimer = 1;
constexpr UINT refreshIntervalMs = 250;
constexpr wchar_t unavailableValue[] = L"—";

struct AxisControls {
    int labelId;
    Text label;
    int stateId;
    int valueId;
};

constexpr std::array axisControls{
    AxisControls{IDC_DIAGNOSTICS_STEERING_LABEL, Text::mainSteering, IDC_DIAGNOSTICS_STEERING_STATE, IDC_DIAGNOSTICS_STEERING_VALUE},
    AxisControls{IDC_DIAGNOSTICS_THROTTLE_LABEL, Text::mainThrottle, IDC_DIAGNOSTICS_THROTTLE_STATE, IDC_DIAGNOSTICS_THROTTLE_VALUE},
    AxisControls{IDC_DIAGNOSTICS_BRAKE_LABEL, Text::mainBrake, IDC_DIAGNOSTICS_BRAKE_STATE, IDC_DIAGNOSTICS_BRAKE_VALUE}
};

const wchar_t* connectionText(DiagnosticConnection connection)
{
    switch (connection) {
    case DiagnosticConnection::unavailable: return tr(Text::DlgDiagnosticsUnavailable);
    case DiagnosticConnection::disconnected: return tr(Text::DlgDiagnosticsDisconnected);
    case DiagnosticConnection::stale: return tr(Text::DlgDiagnosticsStale);
    case DiagnosticConnection::connected: return tr(Text::DlgDiagnosticsConnected);
    }
    return unavailableValue;
}

const wchar_t* modeText(DiagnosticMode mode)
{
    switch (mode) {
    case DiagnosticMode::waitingForDriver: return tr(Text::DlgDiagnosticsWaitingDriver);
    case DiagnosticMode::waitingForVehicleInput: return tr(Text::DlgDiagnosticsWaitingVehicle);
    case DiagnosticMode::gameNotForeground: return tr(Text::DlgDiagnosticsNotForeground);
    case DiagnosticMode::usbDisabled: return tr(Text::DlgDiagnosticsUsbDisabled);
    case DiagnosticMode::profileUnavailable: return tr(Text::DlgDiagnosticsProfileUnavailable);
    case DiagnosticMode::usb: return tr(Text::DlgDiagnosticsUsb);
    case DiagnosticMode::simulation: return tr(Text::DlgDiagnosticsSimulation);
    }
    return unavailableValue;
}

const wchar_t* axisText(DiagnosticAxisState state)
{
    switch (state) {
    case DiagnosticAxisState::unassigned: return tr(Text::DlgDiagnosticsAxisUnassigned);
    case DiagnosticAxisState::deviceUnavailable: return tr(Text::DlgDiagnosticsDeviceUnavailable);
    case DiagnosticAxisState::axisUnavailable: return tr(Text::DlgDiagnosticsAxisUnavailable);
    case DiagnosticAxisState::waitingForNeutral: return tr(Text::DlgDiagnosticsWaitingNeutral);
    case DiagnosticAxisState::armed: return tr(Text::DlgDiagnosticsArmed);
    case DiagnosticAxisState::awaitingProfile: return tr(Text::DlgDiagnosticsAwaitingProfile);
    case DiagnosticAxisState::inactive: return tr(Text::DlgDiagnosticsInactive);
    case DiagnosticAxisState::simulated: return tr(Text::DlgDiagnosticsSimulated);
    case DiagnosticAxisState::sampleExpired: return tr(Text::DlgDiagnosticsSampleExpired);
    }
    return unavailableValue;
}

const wchar_t* accessoryText(DiagnosticAccessoryState state)
{
    switch (state) {
    case DiagnosticAccessoryState::unconfigured: return tr(Text::DlgDiagnosticsAccessoriesUnconfigured);
    case DiagnosticAccessoryState::waiting: return tr(Text::DlgDiagnosticsAccessoriesWaiting);
    case DiagnosticAccessoryState::ready: return tr(Text::DlgDiagnosticsAccessoriesReady);
    case DiagnosticAccessoryState::profileError: return tr(Text::DlgDiagnosticsAccessoriesError);
    case DiagnosticAccessoryState::inactive: return tr(Text::DlgDiagnosticsInactive);
    case DiagnosticAccessoryState::blocked: return tr(Text::DlgDiagnosticsAccessoriesBlocked);
    case DiagnosticAccessoryState::deviceUnavailable: return tr(Text::DlgDiagnosticsDeviceUnavailable);
    case DiagnosticAccessoryState::inputUnavailable: return tr(Text::DlgDiagnosticsAccessoryInputUnavailable);
    case DiagnosticAccessoryState::sampleExpired: return tr(Text::DlgDiagnosticsSampleExpired);
    }
    return unavailableValue;
}

const wchar_t* feedbackText(DiagnosticFeedbackState state)
{
    switch (state) {
    case DiagnosticFeedbackState::disabled: return tr(Text::DlgDiagnosticsFeedbackDisabled);
    case DiagnosticFeedbackState::waiting: return tr(Text::DlgDiagnosticsFeedbackWaiting);
    case DiagnosticFeedbackState::active: return tr(Text::DlgDiagnosticsFeedbackActive);
    case DiagnosticFeedbackState::unavailable: return tr(Text::DlgDiagnosticsFeedbackUnavailable);
    case DiagnosticFeedbackState::inactive: return tr(Text::DlgDiagnosticsInactive);
    }
    return unavailableValue;
}

std::wstring eventAge(std::uint64_t at, std::uint64_t now)
{
    const auto seconds = now >= at ? (now - at) / 1000 : 0;
    if (seconds == 0)
        return tr(Text::DlgDiagnosticsAgeNow);

    std::wstring age;
    if (seconds >= 3600)
        age = std::to_wstring(seconds / 3600) + L" h";
    else if (seconds >= 60)
        age = std::to_wstring(seconds / 60) + L" min";
    else
        age = std::to_wstring(seconds) + L" s";
    return tr(Text::DlgDiagnosticsAgePrefix) + age + tr(Text::DlgDiagnosticsAgeSuffix);
}

std::wstring appliedValue(const DiagnosticAxis& axis, std::uint64_t now)
{
    if (!axis.appliedAt || !std::isfinite(axis.value))
        return unavailableValue;
    const auto percent = std::lround(std::clamp(axis.value, -1.0f, 1.0f) * 100.0f);
    return std::to_wstring(percent) + tr(Text::mainPercentSuffix) + L" · " + eventAge(axis.appliedAt, now);
}

std::wstring profileText(const DiagnosticSnapshot& snapshot)
{
    if (!snapshot.profileLoaded)
        return unavailableValue;
    const auto end = std::ranges::find(snapshot.profile, '\0');
    const std::string_view profile(snapshot.profile.begin(), end);
    if (profile.empty())
        return tr(Text::mainDefaultProfile);
    const auto bytes = static_cast<int>(profile.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, profile.data(), bytes, nullptr, 0);
    if (count <= 0)
        return unavailableValue;
    std::wstring label(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, profile.data(), bytes, label.data(), count) != count)
        return unavailableValue;
    return label;
}

void setChangedText(HWND window, int id, std::wstring_view text)
{
    wchar_t current[384]{};
    const auto count = GetDlgItemTextW(window, id, current, static_cast<int>(std::size(current)));
    if (std::wstring_view(current, count) != text)
        SetDlgItemTextW(window, id, text.data());
}

void refresh(HWND window, DiagnosticReader& reader)
{
    const auto read = reader.read();
    setChangedText(window, IDC_DIAGNOSTICS_CONNECTION, connectionText(read.connection));
    const bool connected = read.connection == DiagnosticConnection::connected;
    const auto& snapshot = read.snapshot;
    const auto now = GetTickCount64();
    setChangedText(window, IDC_DIAGNOSTICS_MODE, connected ? modeText(snapshot.mode) : unavailableValue);
    setChangedText(window, IDC_DIAGNOSTICS_PROFILE, connected ? profileText(snapshot) : unavailableValue);
    for (std::size_t i = 0; i < axisControls.size(); ++i) {
        const auto& controls = axisControls[i];
        const auto& axis = snapshot.axes[i];
        setChangedText(window, controls.stateId, connected ? axisText(axis.state) : unavailableValue);
        setChangedText(window, controls.valueId, connected ? appliedValue(axis, now) : unavailableValue);
    }

    std::wstring accessories = connected ? accessoryText(snapshot.accessories) : unavailableValue;
    if (connected && snapshot.accessoryCount) {
        accessories += L" · " + std::to_wstring(snapshot.accessoryArmed) + L"/" + std::to_wstring(snapshot.accessoryCount);
    }
    setChangedText(window, IDC_DIAGNOSTICS_ACCESSORIES, accessories);
    const auto lastKey = connected && snapshot.accessorySentAt && isAccessoryKey(snapshot.lastAccessoryKey)
        ? accessoryKeyLabel(snapshot.lastAccessoryKey) + L" · " + eventAge(snapshot.accessorySentAt, now)
        : unavailableValue;
    setChangedText(window, IDC_DIAGNOSTICS_ACCESSORY, lastKey);
    setChangedText(window, IDC_DIAGNOSTICS_FEEDBACK, connected ? feedbackText(snapshot.feedback) : unavailableValue);
}

INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        SetWindowTextW(window, tr(Text::DlgDiagnosticsTitle));
        SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(Text::DlgDiagnosticsIntro));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_MODE_LABEL, tr(Text::DlgDiagnosticsMode));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_PROFILE_LABEL, tr(Text::DlgDiagnosticsProfile));
        SetDlgItemTextW(window, IDC_MODAL_LIST_HEADING, tr(Text::DlgDiagnosticsAxes));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_STATE_LABEL, tr(Text::DlgDiagnosticsAxisState));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_LAST_VALUE_LABEL, tr(Text::DlgDiagnosticsLastValue));
        for (const auto& axis : axisControls)
            SetDlgItemTextW(window, axis.labelId, tr(axis.label));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_ACCESSORIES_LABEL, tr(Text::DlgDiagnosticsAccessories));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_ACCESSORY_LABEL, tr(Text::DlgDiagnosticsLastKey));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_FEEDBACK_LABEL, tr(Text::DlgDiagnosticsFeedback));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgDiagnosticsClose));
        DialogTheme::attach(window, tr(Text::DlgDiagnosticsHeading), IDC_DIAGNOSTICS_CONNECTION, 0, {IDCANCEL});
        refresh(window, *reinterpret_cast<DiagnosticReader*>(lParam));
        SetTimer(window, refreshTimer, refreshIntervalMs, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wParam == refreshTimer) {
            if (const auto reader = reinterpret_cast<DiagnosticReader*>(GetWindowLongPtrW(window, DWLP_USER)))
                refresh(window, *reader);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) != IDCANCEL)
            break;
        [[fallthrough]];
    case WM_CLOSE:
        EndDialog(window, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        KillTimer(window, refreshTimer);
        break;
    }
    return FALSE;
}

} // namespace

void showDiagnostics(HWND owner, const std::filesystem::path& gameRoot)
{
    DiagnosticReader reader(gameRoot);
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DIAGNOSTICS), owner,
                                       dialogProc, reinterpret_cast<LPARAM>(&reader));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgDiagnosticsOpenError), tr(Text::DlgDiagnosticsTitle), MB_OK | MB_ICONERROR);
}

} // namespace chrome_axis
