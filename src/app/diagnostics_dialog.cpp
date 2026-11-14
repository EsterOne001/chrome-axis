#include "diagnostics_dialog.hpp"

#include "app_text.hpp"
#include "chrome_axis/accessories.hpp"
#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/input.hpp"
#include "chrome_axis/presets.hpp"
#include "dialog_theme.hpp"

#include <winver.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace chrome_axis {

const wchar_t* missingPluginFilesText(PluginFiles files) noexcept
{
    if (!files.library)
        return tr(files.script ? Text::DlgDiagnosticsMissingLibrary : Text::DlgDiagnosticsMissingBoth);
    if (!files.script)
        return tr(Text::DlgDiagnosticsMissingScript);
    return nullptr;
}

namespace {

constexpr UINT_PTR refreshTimer = 1;
constexpr UINT refreshIntervalMs = 250;
constexpr UINT_PTR copyTimer = 2;
constexpr UINT copyFeedbackMs = 2500;
constexpr wchar_t unavailableValue[] = L"—";

struct DialogState {
    DiagnosticReader reader;
    const std::filesystem::path& gameRoot;
    const std::filesystem::path& profileRoot;
    std::wstring models;
    const wchar_t* missingFiles{};
    std::wstring pluginVersion{};
    std::wstring versionMismatch{};
};

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
    case DiagnosticAxisState::temporarilyUnavailable: return tr(Text::DlgDiagnosticsTemporaryRead);
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
    case DiagnosticAccessoryState::temporarilyUnavailable: return tr(Text::DlgDiagnosticsTemporaryRead);
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

std::wstring percentText(float value)
{
    if (!std::isfinite(value))
        return unavailableValue;
    const auto percent = std::lround(std::clamp(value, -1.0f, 1.0f) * 100.0f);
    return std::to_wstring(percent) + tr(Text::mainPercentSuffix);
}

std::wstring appliedValue(const DiagnosticAxis& axis, std::uint64_t now)
{
    if (!axis.appliedAt || !std::isfinite(axis.value))
        return unavailableValue;
    return percentText(axis.value) + L" · " + eventAge(axis.appliedAt, now);
}

std::wstring fromUtf8(std::string_view text)
{
    if (text.empty())
        return {};
    const auto bytes = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), bytes, nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring label(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), bytes, label.data(), count) != count)
        return {};
    return label;
}

std::wstring profileText(const DiagnosticSnapshot& snapshot)
{
    if (!snapshot.profileLoaded)
        return unavailableValue;
    const auto end = std::ranges::find(snapshot.profile, '\0');
    if (end == snapshot.profile.begin())
        return tr(Text::mainDefaultProfile);
    const auto label = fromUtf8(std::string_view(snapshot.profile.begin(), end));
    return label.empty() ? unavailableValue : label;
}

std::wstring reportLabel(std::wstring text)
{
    std::ranges::replace_if(text, [](wchar_t character) {
        return character < L' ' || (character >= 0x7f && character <= 0x9f) ||
            (character >= 0x2028 && character <= 0x202e) ||
            (character >= 0x2066 && character <= 0x2069);
    }, L' ');
    if (const auto first = text.find_first_not_of(L' '); first != std::wstring::npos)
        text = text.substr(first, text.find_last_not_of(L' ') - first + 1);
    else
        return {};
    constexpr std::size_t limit = 160;
    if (text.size() > limit) {
        text.resize(limit);
        if (text.back() >= 0xd800 && text.back() <= 0xdbff)
            text.pop_back();
        text += L'…';
    }
    return text;
}

std::wstring fileVersion(const std::filesystem::path& path, bool omitZeroRevision = false)
{
    DWORD unused{};
    const auto bytes = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (bytes == 0)
        return unavailableValue;
    std::vector<std::byte> buffer(bytes);
    void* data{};
    UINT size{};
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, buffer.data()) ||
        !VerQueryValueW(buffer.data(), L"\\", &data, &size) || size < sizeof(VS_FIXEDFILEINFO))
        return unavailableValue;
    const auto& info = *static_cast<const VS_FIXEDFILEINFO*>(data);
    if (info.dwSignature != 0xfeef04bd)
        return unavailableValue;
    auto version = std::to_wstring(HIWORD(info.dwFileVersionMS)) + L'.' +
        std::to_wstring(LOWORD(info.dwFileVersionMS)) + L'.' + std::to_wstring(HIWORD(info.dwFileVersionLS));
    if (!omitZeroRevision || LOWORD(info.dwFileVersionLS))
        version += L'.' + std::to_wstring(LOWORD(info.dwFileVersionLS));
    return version;
}

std::wstring detectedModels(std::span<const DeviceInfo> devices)
{
    constexpr std::size_t modelLimit = 12;
    std::vector<std::wstring> unique;
    std::wstring models;
    for (const auto& device : devices) {
        if (device.productName.find('\\') != std::string::npos || device.productName.starts_with('/') ||
            device.productName.find(":/") != std::string::npos || isDeviceId(device.productName))
            continue;
        auto model = reportLabel(fromUtf8(device.productName));
        if (model.empty() || std::ranges::find(unique, model) != unique.end())
            continue;
        if (!models.empty())
            models += L" · ";
        if (unique.size() == modelLimit) {
            models += L'…';
            break;
        }
        models += model;
        unique.push_back(std::move(model));
    }
    return models;
}

std::wstring accessoryStatus(const DiagnosticSnapshot& snapshot)
{
    std::wstring text = accessoryText(snapshot.accessories);
    if (snapshot.accessoryCount)
        text += L" · " + std::to_wstring(snapshot.accessoryArmed) + L"/" + std::to_wstring(snapshot.accessoryCount);
    return text;
}

std::wstring lastAccessoryKey(const DiagnosticSnapshot& snapshot, std::uint64_t now)
{
    return snapshot.accessorySentAt && isAccessoryKey(snapshot.lastAccessoryKey)
        ? accessoryKeyLabel(snapshot.lastAccessoryKey) + L" · " + eventAge(snapshot.accessorySentAt, now)
        : unavailableValue;
}

std::wstring diagnosticReport(const DiagnosticRead& read, const DialogState& state, std::uint64_t now)
{
    std::wstring report = tr(Text::DlgDiagnosticsReport);
    report += L"\r\n";
    const auto line = [&report](Text label, std::wstring_view value) {
        report += tr(label);
        report += L" — ";
        report += value.empty() ? unavailableValue : value;
        report += L"\r\n";
    };
    line(Text::DlgDiagnosticsReportApp, CHROME_AXIS_VERSION_WIDE);
    if (!state.gameRoot.empty()) {
        line(Text::DlgDiagnosticsReportGame, fileVersion(state.gameRoot / L"bin" / L"x64" / L"Cyberpunk2077.exe"));
        line(Text::DlgDiagnosticsReportPlugin, state.pluginVersion);
    }
    if (state.missingFiles) {
        report += state.missingFiles;
        report += L"\r\n";
    }
    if (!state.versionMismatch.empty())
        report += state.versionMismatch + L"\r\n";
    report += connectionText(read.connection);
    report += L"\r\n";
    const auto localProfile = activePreset(state.profileRoot);
    if (!localProfile.directory.empty())
        line(Text::DlgDiagnosticsReportLocalProfile,
             localProfile.name.empty() ? tr(Text::mainDefaultProfile) : reportLabel(localProfile.name));
    line(Text::DlgDiagnosticsReportModels, state.models);

    if (read.sampledAt) {
        const auto age = eventAge(read.sampledAt, now);
        line(Text::DlgDiagnosticsMode, std::wstring(modeText(read.snapshot.mode)) + L" · " + age);
        line(Text::DlgDiagnosticsReportPluginProfile, reportLabel(profileText(read.snapshot)));
    }
    report += L"\r\n";
    if (read.lastDriveAt)
        line(Text::DlgDiagnosticsReportLastDrive, std::wstring(modeText(read.lastDrive.mode)) +
             L" · " + eventAge(read.lastDriveAt, now));
    else
        report += std::wstring(tr(Text::DlgDiagnosticsReportNoDrive)) + L"\r\n";
    if (read.lastDriveAt || read.sampledAt) {
        const auto& snapshot = read.lastDriveAt ? read.lastDrive : read.snapshot;
        if (read.lastDriveAt)
            line(Text::DlgDiagnosticsProfile, reportLabel(profileText(snapshot)));
        for (std::size_t i = 0; i < axisControls.size(); ++i) {
            const auto& axis = snapshot.axes[i];
            std::wstring detail = axisText(axis.state);
            detail += L" · " + std::wstring(tr(Text::DlgDiagnosticsReportRead)) + L" " +
                      (axis.inputAvailable ? percentText(axis.inputValue) : unavailableValue);
            detail += L" · " + std::wstring(tr(Text::DlgDiagnosticsReportRequested)) + L" " +
                      (axis.requestedAvailable ? percentText(axis.requestedValue) : unavailableValue);
            detail += L" · " + std::wstring(tr(Text::DlgDiagnosticsReportWritten)) + L" " + appliedValue(axis, now);
            if (axis.delivery == DiagnosticDelivery::applied)
                detail += L" · " + std::wstring(tr(Text::DlgDiagnosticsReportApplied));
            if (axis.delivery == DiagnosticDelivery::gameKept)
                detail += L" · " + std::wstring(tr(Text::DlgDiagnosticsReportGameKept));
            line(axisControls[i].label, detail);
        }
        line(Text::DlgDiagnosticsAccessories, accessoryStatus(snapshot));
        line(Text::DlgDiagnosticsLastKey, lastAccessoryKey(snapshot, now));
        line(Text::DlgDiagnosticsFeedback, feedbackText(snapshot.feedback));
    }
    const auto events = std::span(read.events).first(std::min<std::size_t>(read.eventCount, read.events.size()));
    if (!events.empty()) {
        report += L"\r\n";
        report += tr(Text::DlgDiagnosticsReportEvents);
        report += L"\r\n";
    }
    const DiagnosticEvent* previous{};
    for (const auto& event : events) {
        report += L"- " + eventAge(event.sampledAt, now) + L" — " + modeText(event.mode);
        const auto change = [&report](Text label, const wchar_t* value) {
            report += L" · " + std::wstring(tr(label)) + L" " + value;
        };
        for (std::size_t i = 0; i < axisControls.size(); ++i) {
            if (!previous || event.axes[i] != previous->axes[i])
                change(axisControls[i].label, axisText(event.axes[i]));
        }
        if (!previous || event.accessories != previous->accessories)
            change(Text::DlgDiagnosticsAccessories, accessoryText(event.accessories));
        if (!previous || event.feedback != previous->feedback)
            change(Text::DlgDiagnosticsFeedback, feedbackText(event.feedback));
        report += L"\r\n";
        previous = &event;
    }
    return report;
}

bool copyToClipboard(HWND owner, const std::wstring& text)
{
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        return false;
    const auto contents = GlobalLock(memory);
    if (!contents) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(contents, text.c_str(), bytes);
    GlobalUnlock(memory);
    bool copied = false;
    if (OpenClipboard(owner)) {
        copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
        CloseClipboard();
    }
    if (!copied)
        GlobalFree(memory);
    return copied;
}

void setChangedText(HWND window, int id, std::wstring_view text)
{
    wchar_t current[384]{};
    const auto count = GetDlgItemTextW(window, id, current, static_cast<int>(std::size(current)));
    if (std::wstring_view(current, count) != text)
        SetDlgItemTextW(window, id, text.data());
}

void refresh(HWND window, DialogState& state)
{
    const auto read = state.reader.read();
    setChangedText(window, IDC_DIAGNOSTICS_CONNECTION, state.missingFiles ? state.missingFiles :
        !state.versionMismatch.empty() ? state.versionMismatch.c_str() : connectionText(read.connection));
    const bool connected = read.connection == DiagnosticConnection::connected;
    const auto mode = read.snapshot.mode;
    const bool showLastDrive = read.lastDriveAt && (!connected || mode == DiagnosticMode::gameNotForeground ||
        mode == DiagnosticMode::waitingForDriver);
    const auto& snapshot = showLastDrive ? read.lastDrive : read.snapshot;
    const bool showControls = connected || showLastDrive;
    const auto now = GetTickCount64();
    setChangedText(window, IDC_DIAGNOSTICS_MODE, connected ? modeText(mode) : unavailableValue);
    setChangedText(window, IDC_DIAGNOSTICS_PROFILE_LABEL,
                   tr(showLastDrive ? Text::DlgDiagnosticsLastDriveProfile : Text::DlgDiagnosticsProfile));
    setChangedText(window, IDC_DIAGNOSTICS_PROFILE, showControls ? profileText(snapshot) : unavailableValue);
    setChangedText(window, IDC_MODAL_LIST_HEADING,
                   tr(showLastDrive ? Text::DlgDiagnosticsLastDriveHeading : Text::DlgDiagnosticsAxes));
    for (std::size_t i = 0; i < axisControls.size(); ++i) {
        const auto& controls = axisControls[i];
        const auto& axis = snapshot.axes[i];
        setChangedText(window, controls.stateId, showControls ? axisText(axis.state) : unavailableValue);
        setChangedText(window, controls.valueId, showControls ? appliedValue(axis, now) : unavailableValue);
    }

    setChangedText(window, IDC_DIAGNOSTICS_ACCESSORIES, showControls ? accessoryStatus(snapshot) : unavailableValue);
    setChangedText(window, IDC_DIAGNOSTICS_ACCESSORY, showControls ? lastAccessoryKey(snapshot, now) : unavailableValue);
    setChangedText(window, IDC_DIAGNOSTICS_FEEDBACK, showControls ? feedbackText(snapshot.feedback) : unavailableValue);
}

INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    const auto state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        auto& initial = *reinterpret_cast<DialogState*>(lParam);
        SetWindowTextW(window, tr(Text::DlgDiagnosticsTitle));
        SetDlgItemTextW(window, IDC_MODAL_INTRO,
                       tr(initial.missingFiles || !initial.versionMismatch.empty()
                           ? Text::DlgDiagnosticsRedeploy : Text::DlgDiagnosticsIntro));
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
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_COPY, tr(Text::DlgDiagnosticsCopy));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgDiagnosticsClose));
        DialogTheme::attach(window, tr(Text::DlgDiagnosticsHeading), IDC_DIAGNOSTICS_CONNECTION, 0, {IDCANCEL});
        refresh(window, initial);
        SetTimer(window, refreshTimer, refreshIntervalMs, nullptr);
        SetFocus(GetDlgItem(window, IDCANCEL));
        return FALSE;
    }
    case WM_TIMER:
        if (wParam == refreshTimer && state) {
            refresh(window, *state);
            return TRUE;
        }
        if (wParam == copyTimer) {
            KillTimer(window, copyTimer);
            SetDlgItemTextW(window, IDC_DIAGNOSTICS_COPY, tr(Text::DlgDiagnosticsCopy));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_DIAGNOSTICS_COPY && state) {
            const auto read = state->reader.read();
            const bool copied = copyToClipboard(window, diagnosticReport(read, *state, GetTickCount64()));
            if (SetTimer(window, copyTimer, copyFeedbackMs, nullptr))
                SetDlgItemTextW(window, IDC_DIAGNOSTICS_COPY,
                                tr(copied ? Text::DlgDiagnosticsCopied : Text::DlgDiagnosticsCopyError));
            return TRUE;
        }
        if (LOWORD(wParam) != IDCANCEL)
            break;
        [[fallthrough]];
    case WM_CLOSE:
        EndDialog(window, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        KillTimer(window, refreshTimer);
        KillTimer(window, copyTimer);
        break;
    }
    return FALSE;
}

} // namespace

void showDiagnostics(HWND owner, const std::filesystem::path& profileRoot, std::span<const DeviceInfo> devices)
{
    const auto pluginDirectory = profileRoot.parent_path();
    const auto gameRoot = installedGameRoot(pluginDirectory);
    DialogState state{DiagnosticReader(gameRoot), gameRoot, profileRoot, detectedModels(devices)};
    if (!gameRoot.empty()) {
        const auto files = pluginFiles(pluginDirectory);
        state.missingFiles = missingPluginFilesText(files);
        state.pluginVersion = files.library ? fileVersion(pluginDirectory / L"ChromeAxis.dll", true) : unavailableValue;
        if (state.pluginVersion != unavailableValue && state.pluginVersion != CHROME_AXIS_VERSION_WIDE) {
            state.versionMismatch = std::wstring(tr(Text::DlgDiagnosticsVersionMismatch)) + L" · " +
                tr(Text::DlgDiagnosticsReportApp) + L" " CHROME_AXIS_VERSION_WIDE L" / DLL " + state.pluginVersion;
        }
    }
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DIAGNOSTICS), owner,
                                       dialogProc, reinterpret_cast<LPARAM>(&state));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgDiagnosticsOpenError), tr(Text::DlgDiagnosticsTitle), MB_OK | MB_ICONERROR);
}

} // namespace chrome_axis
