#include "diagnostics_dialog.hpp"

#include "app_text.hpp"
#include "chrome_axis/accessories.hpp"
#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/input.hpp"
#include "chrome_axis/presets.hpp"
#include "diagnostic_recording.hpp"
#include "dialog_theme.hpp"
#include "../core/settings_replace.hpp"

#include <winver.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
constexpr UINT refreshIntervalMs = 150;
constexpr UINT_PTR copyTimer = 2;
constexpr UINT copyFeedbackMs = 2500;
constexpr wchar_t unavailableValue[] = L"—";
constexpr auto releaseCom = [](auto* object) { object->Release(); };

struct DialogState {
    DiagnosticReader& reader;
    DiagnosticRecorder& recorder;
    const std::filesystem::path& gameRoot;
    const std::filesystem::path& profileRoot;
    std::wstring models;
    std::span<const DeviceInfo> devices;
    const wchar_t* missingFiles{};
    std::wstring pluginVersion{};
    std::wstring versionMismatch{};
};

struct AccessoryDialogState {
    DiagnosticReader& reader;
    std::vector<std::wstring> rowLabels;
};

struct AccessoryView {
    const DiagnosticSnapshot& snapshot;
    std::uint64_t sampledAt;
    bool historical;

    std::span<const DiagnosticAccessoryCommand> commands() const noexcept
    {
        if (!sampledAt)
            return {};
        return std::span(snapshot.accessoryCommands).first(
            std::min<std::size_t>(snapshot.accessoryCount, snapshot.accessoryCommands.size()));
    }
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

struct PerformanceControls {
    Text label;
    int labelId;
    int valueId;
    PerformanceTiming PerformanceReport::*timing;
};

constexpr std::array performanceControls{
    PerformanceControls{Text::DlgDiagnosticsPerformanceUSB, IDC_DIAGNOSTICS_PERFORMANCE_USB_LABEL,
        IDC_DIAGNOSTICS_PERFORMANCE_USB, &PerformanceReport::usbRead},
    PerformanceControls{Text::DlgDiagnosticsPerformanceFeedback, IDC_DIAGNOSTICS_PERFORMANCE_FEEDBACK_LABEL,
        IDC_DIAGNOSTICS_PERFORMANCE_FEEDBACK, &PerformanceReport::feedback},
    PerformanceControls{Text::DlgDiagnosticsPerformanceAge, IDC_DIAGNOSTICS_PERFORMANCE_AGE_LABEL,
        IDC_DIAGNOSTICS_PERFORMANCE_AGE, &PerformanceReport::inputAge},
    PerformanceControls{Text::DlgDiagnosticsPerformanceMotion, IDC_DIAGNOSTICS_PERFORMANCE_MOTION_LABEL,
        IDC_DIAGNOSTICS_PERFORMANCE_MOTION, &PerformanceReport::motionAge}
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
    case DiagnosticAccessoryState::axisConflict: return tr(Text::DlgDiagnosticsAccessoryAxisConflict);
    case DiagnosticAccessoryState::inactive: return tr(Text::DlgDiagnosticsInactive);
    case DiagnosticAccessoryState::blocked: return tr(Text::DlgDiagnosticsAccessoriesBlocked);
    case DiagnosticAccessoryState::modifierBlocked: return tr(Text::DlgDiagnosticsAccessoriesModifier);
    case DiagnosticAccessoryState::selectorAmbiguous: return tr(Text::DlgDiagnosticsTransmissionAmbiguous);
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
    case DiagnosticFeedbackState::neutral: return tr(Text::DlgDiagnosticsFeedbackNeutral);
    case DiagnosticFeedbackState::waitingForMotion: return tr(Text::DlgDiagnosticsFeedbackWaitingMotion);
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

std::wstring precisePercentText(float value, bool showSign = true)
{
    if (!std::isfinite(value))
        return unavailableValue;
    const auto percent = std::round(static_cast<double>(value) * 1000.0) / 10.0;
    if (percent == 0.0)
        return percentText(0.0f);
    auto text = std::format(L"{:.1f}", percent);
    if (text.ends_with(L".0"))
        text.resize(text.size() - 2);
    else
        text[text.find(L'.')] = tr(Text::mainDecimalSeparator)[0];
    return (showSign && percent > 0.0 ? L"+" : L"") + text + tr(Text::mainPercentSuffix);
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

std::wstring reportDeviceModel(const DeviceInfo& device)
{
    const std::string_view name = device.productName;
    if (name.find('\\') != name.npos || name.starts_with('/') || name.find(":/") != name.npos)
        return {};

    // Product names are usually generic, but some drivers append an instance GUID.
    constexpr std::size_t guidLength = 36;
    for (auto dash = name.find('-'); dash != name.npos; dash = name.find('-', dash + 1)) {
        if (dash < 8 || name.size() - (dash - 8) < guidLength)
            continue;
        const auto candidate = name.substr(dash - 8, guidLength);
        if (candidate[13] == '-' && candidate[18] == '-' && candidate[23] == '-' &&
            candidate.find_first_not_of("0123456789abcdefABCDEF-") == candidate.npos &&
            std::ranges::count(candidate, '-') == 4)
            return {};
    }
    return reportLabel(fromUtf8(name));
}

std::wstring detectedModels(std::span<const DeviceInfo> devices)
{
    constexpr std::size_t modelLimit = 12;
    std::vector<std::wstring> unique;
    std::wstring models;
    for (const auto& device : devices) {
        auto model = reportDeviceModel(device);
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

AccessoryView accessoryView(const DiagnosticRead& read)
{
    const bool connected = read.connection == DiagnosticConnection::connected;
    const auto mode = read.snapshot.mode;
    if (read.lastDriveAt && (!connected || mode == DiagnosticMode::gameNotForeground || mode == DiagnosticMode::waitingForDriver))
        return {read.lastDrive, read.lastDriveAt, true};
    return {read.snapshot, connected ? read.sampledAt : 0, false};
}

std::wstring accessoryViewContext(const AccessoryView& view, std::uint64_t now)
{
    const auto commands = view.commands();
    if (commands.empty()) {
        if (view.sampledAt && view.snapshot.accessories == DiagnosticAccessoryState::unconfigured)
            return tr(Text::DlgDiagnosticsAccessoriesUnconfigured);
        if (view.sampledAt && view.snapshot.accessories == DiagnosticAccessoryState::profileError)
            return tr(Text::DlgDiagnosticsAccessoriesError);
        return tr(Text::DlgDiagnosticsCommandNoData);
    }
    std::wstring text = tr(view.historical ? Text::DlgDiagnosticsCommandHistory : Text::DlgDiagnosticsCommandCurrent);
    if (view.historical)
        text += L" · " + eventAge(view.sampledAt, now);
    if (view.snapshot.profileLoaded)
        text += L" · " + reportLabel(profileText(view.snapshot));
    return text;
}

const wchar_t* commandState(DiagnosticCommandState state)
{
    switch (state) {
    case DiagnosticCommandState::inactive: return tr(Text::DlgDiagnosticsInactive);
    case DiagnosticCommandState::waitingForVehicle: return tr(Text::DlgDiagnosticsWaitingVehicle);
    case DiagnosticCommandState::waitingForNeutral: return tr(Text::DlgDiagnosticsWaitingNeutral);
    case DiagnosticCommandState::ready: return tr(Text::DlgDiagnosticsArmed);
    case DiagnosticCommandState::deviceUnavailable: return tr(Text::DlgDiagnosticsDeviceUnavailable);
    case DiagnosticCommandState::inputUnavailable: return tr(Text::DlgDiagnosticsAccessoryInputUnavailable);
    case DiagnosticCommandState::temporarilyUnavailable: return tr(Text::DlgDiagnosticsTemporaryRead);
    case DiagnosticCommandState::sampleExpired: return tr(Text::DlgDiagnosticsSampleExpired);
    }
    return unavailableValue;
}

std::wstring commandOutput(const DiagnosticAccessoryCommand& command)
{
    if (command.handbrakeCeilingPercent) {
        if (!command.handbrakeAvailable || !std::isfinite(command.handbrakeRequested) ||
            !std::isfinite(command.handbrakeFinal)) {
            return unavailableValue;
        }
        return std::wstring(tr(Text::DlgDiagnosticsHandbrakeRequested)) + L" " + precisePercentText(command.handbrakeRequested, false)
            + L" · " + tr(Text::DlgDiagnosticsHandbrakeFinal) + L" " + precisePercentText(command.handbrakeFinal, false);
    }
    switch (command.output) {
    case DiagnosticCommandOutput::nativeReady: return tr(Text::DlgDiagnosticsNativeReady);
    case DiagnosticCommandOutput::nativePending: return tr(Text::DlgDiagnosticsNativePending);
    case DiagnosticCommandOutput::nativeEngaged: return tr(Text::DlgDiagnosticsNativeEngaged);
    case DiagnosticCommandOutput::nativeRefused: return tr(Text::DlgDiagnosticsNativeRefused);
    case DiagnosticCommandOutput::nativePaused: return tr(Text::DlgDiagnosticsNativePaused);
    case DiagnosticCommandOutput::none: return tr(Text::DlgDiagnosticsCommandOutputIdle);
    case DiagnosticCommandOutput::held: return tr(Text::DlgDiagnosticsCommandOutputHeld);
    case DiagnosticCommandOutput::pulsing: return tr(Text::DlgDiagnosticsCommandOutputPulsing);
    case DiagnosticCommandOutput::blocked: return tr(Text::DlgDiagnosticsCommandOutputBlocked);
    case DiagnosticCommandOutput::failed: return tr(Text::DlgDiagnosticsCommandOutputFailed);
    }
    return unavailableValue;
}

std::wstring commandName(const DiagnosticAccessoryCommand& command)
{
    bool validInput = false;
    switch (command.kind) {
    case AccessoryKind::Button:
        validInput = command.index < buttonCount;
        break;
    case AccessoryKind::Pov:
        validInput = command.index < povCount && command.engaged >= 0 &&
            command.engaged < povFullTurn && command.engaged % povDirectionStep == 0;
        break;
    case AccessoryKind::Axis:
        validInput = command.index < axisCount;
        break;
    }
    const AccessoryBinding binding{
        .deviceId = {},
        .kind = command.kind,
        .index = command.index,
        .rest = command.rest,
        .engaged = command.engaged,
        .role = command.role <= ShifterRole::Downshift && command.role >= ShifterRole::None ? command.role : ShifterRole::None,
    };
    const auto input = validInput ? accessoryBindingInputLabel(binding) : unavailableValue;
    if (command.handbrakeCeilingPercent) {
        return input + L" → " + tr(Text::DlgDiagnosticsHandbrake) + L" · "
            + tr(Text::DlgDiagnosticsHandbrakeCeiling) + L" " + std::to_wstring(command.handbrakeCeilingPercent) + L" %";
    }
    if (command.key == 0 && binding.role >= ShifterRole::Gear1 && binding.role <= ShifterRole::Reverse)
        return input + L" → " + tr(Text::DlgShifterNativeOutput);
    const auto key = isAccessoryKey(command.key) ? accessoryKeyLabel(command.key) : unavailableValue;
    return input + L" → " + key + L" · " + accessoryActivationLabel(command.activation);
}

std::wstring commandStateWithActivity(const DiagnosticAccessoryCommand& command)
{
    std::wstring text = commandState(command.state);
    if (command.activeAvailable)
        text += L" · " + std::wstring(tr(command.active ? Text::DlgDiagnosticsCommandPressed : Text::DlgDiagnosticsCommandReleased));
    return text;
}

std::wstring commandSignal(const DiagnosticAccessoryCommand& command)
{
    auto text = commandStateWithActivity(command);
    if (command.travelAvailable && std::isfinite(command.travel)) {
        text += L" · " + std::wstring(tr(Text::DlgDiagnosticsCommandTravel)) + L" " + percentText(command.travel);
    }
    return text;
}

std::wstring commandSent(const DiagnosticAccessoryCommand& command, std::uint64_t now)
{
    return command.lastSentAt ? eventAge(command.lastSentAt, now) : unavailableValue;
}

std::wstring milliseconds(double micros)
{
    auto formatted = std::format(L"{:.3f}", micros / 1000.0);
    const auto decimal = formatted.find(L'.');
    formatted[decimal] = tr(Text::mainDecimalSeparator)[0];
    return formatted + L" ms";
}

std::string_view usbOperation(const DiagnosticUsbEvent& event)
{
    return {event.operation.begin(), std::ranges::find(event.operation, '\0')};
}

bool slowFeedbackEvent(const DiagnosticUsbEvent& event)
{
    return event.kind == DiagnosticUsbEventKind::deviceSlow && usbOperation(event).starts_with("FFB ");
}

const wchar_t* usbEventTitle(const DiagnosticUsbEvent& event)
{
    switch (event.kind) {
    case DiagnosticUsbEventKind::deviceSlow:
        return tr(slowFeedbackEvent(event) ? Text::DlgDiagnosticsFeedbackEventSlow : Text::DlgDiagnosticsUsbEventSlow);
    case DiagnosticUsbEventKind::devicePending: return tr(Text::DlgDiagnosticsUsbEventPending);
    case DiagnosticUsbEventKind::deviceUnavailable: return tr(Text::DlgDiagnosticsDeviceUnavailable);
    case DiagnosticUsbEventKind::deviceRecovered: return tr(Text::DlgDiagnosticsUsbEventRecovered);
    case DiagnosticUsbEventKind::pollExpired: return tr(Text::DlgDiagnosticsUsbEventPollExpired);
    case DiagnosticUsbEventKind::pollGap: return tr(Text::DlgDiagnosticsUsbEventPollGap);
    case DiagnosticUsbEventKind::commandsWaiting: return tr(Text::DlgDiagnosticsUsbEventCommandsWaiting);
    case DiagnosticUsbEventKind::commandsReady: return tr(Text::DlgDiagnosticsUsbEventCommandsReady);
    case DiagnosticUsbEventKind::contextChanged: return tr(Text::DlgDiagnosticsUsbEventContext);
    case DiagnosticUsbEventKind::menuPaused: return tr(Text::DlgDiagnosticsUsbEventMenu);
    case DiagnosticUsbEventKind::directInputReopened: return tr(Text::DlgDiagnosticsUsbEventReopened);
    }
    return unavailableValue;
}

std::wstring usbBlockerName(const DiagnosticUsbBlocker& blocker)
{
    if (blocker.clutch)
        return tr(Text::DlgDiagnosticsClutch);
    if (blocker.progressiveHandbrake)
        return tr(Text::DlgDiagnosticsHandbrake);
    if (blocker.role > ShifterRole::None && blocker.role <= ShifterRole::Downshift)
        return shifterRoleLabel(blocker.role);
    if (isAccessoryKey(blocker.key))
        return accessoryKeyLabel(blocker.key);

    switch (blocker.kind) {
    case AccessoryKind::Button:
        if (blocker.index < buttonCount)
            return tr(Text::DlgInputButton) + std::to_wstring(blocker.index + 1);
        break;
    case AccessoryKind::Pov:
        if (blocker.index < povCount)
            return tr(Text::DlgInputDpad) + std::to_wstring(blocker.index + 1);
        break;
    case AccessoryKind::Axis:
        if (blocker.index < axisCount)
            return tr(Text::DlgInputAxis) + fromUtf8(axisName(blocker.index));
        break;
    }
    return unavailableValue;
}

std::wstring usbEventDetails(const DiagnosticUsbEvent& event, std::span<const DeviceInfo> devices)
{
    std::wstring detail;
    const auto append = [&](std::wstring_view text) {
        detail += L" · ";
        detail += text;
    };
    const auto bounded = [](const auto& text) {
        return std::string_view(text.begin(), std::ranges::find(text, '\0'));
    };
    const auto operation = usbOperation(event);
    switch (event.kind) {
    case DiagnosticUsbEventKind::deviceSlow:
    case DiagnosticUsbEventKind::devicePending:
    case DiagnosticUsbEventKind::deviceUnavailable:
    case DiagnosticUsbEventKind::deviceRecovered: {
        const auto id = bounded(event.deviceId);
        const auto device = std::ranges::find(devices, id, &DeviceInfo::id);
        const auto model = device == devices.end() ? std::wstring{} : reportDeviceModel(*device);
        const auto fallback = id.empty() ? Text::DlgDiagnosticsUsbEventInterface : Text::DlgDiagnosticsUsbEventDevice;
        append(model.empty() ? std::wstring_view(tr(fallback)) : std::wstring_view(model));
        for (std::size_t index = 0; index < axisControls.size(); ++index) {
            if (event.deviceAxisMask & (1u << index))
                append(tr(axisControls[index].label));
        }
        if (event.deviceAccessoryCount)
            append(std::wstring(tr(Text::DlgDiagnosticsAccessories)) + L" " + std::to_wstring(event.deviceAccessoryCount));
        if (!operation.empty() && std::ranges::all_of(operation, [](char character) {
            return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') || character == ' ';
        }))
            append(fromUtf8(operation));
        if (event.kind == DiagnosticUsbEventKind::deviceUnavailable || event.kind == DiagnosticUsbEventKind::devicePending)
            append(std::format(L"0x{:08X}", event.result));
        break;
    }
    case DiagnosticUsbEventKind::commandsWaiting:
    case DiagnosticUsbEventKind::commandsReady:
        for (std::size_t index = 0; index < axisControls.size(); ++index)
            append(std::wstring(tr(axisControls[index].label)) + L" " + axisText(event.axes[index]));
        if (event.accessoryCount)
            append(std::wstring(tr(Text::DlgDiagnosticsAccessories)) + L" " + accessoryText(event.accessories) +
                   L" " + std::to_wstring(event.accessoryArmed) + L"/" + std::to_wstring(event.accessoryCount));
        if (event.kind == DiagnosticUsbEventKind::commandsWaiting && event.blockerCount) {
            append(tr(Text::DlgDiagnosticsUsbWaitingCommands));
            const auto count = std::min<std::size_t>(event.blockerCount, event.blockers.size());
            for (const auto& blocker : std::span(event.blockers).first(count))
                append(usbBlockerName(blocker) + L" (" + commandState(blocker.state) + L")");
            if (event.blockerCount > count)
                append(L"+" + std::to_wstring(event.blockerCount - count));
        }
        break;
    case DiagnosticUsbEventKind::contextChanged:
        append(modeText(event.mode));
        break;
    case DiagnosticUsbEventKind::directInputReopened:
        if (operation == "driving_session")
            append(tr(Text::DlgDiagnosticsUsbReasonSession));
        else if (operation == "driving_context")
            append(tr(Text::DlgDiagnosticsUsbReasonContext));
        else if (operation == "window")
            append(tr(Text::DlgDiagnosticsUsbReasonWindow));
        else if (operation == "configuration")
            append(tr(Text::DlgDiagnosticsUsbReasonConfiguration));
        break;
    default:
        break;
    }
    if (event.durationMicros || event.kind == DiagnosticUsbEventKind::pollExpired)
        append(milliseconds(static_cast<double>(event.durationMicros)));
    return detail;
}

std::wstring feedbackStatus(const DiagnosticSnapshot& snapshot)
{
    std::wstring status = feedbackText(snapshot.feedback);
    const auto& data = snapshot.feedbackData;
    if (data.motionAvailable && data.airborne)
        status += L" · " + std::wstring(tr(Text::DlgDiagnosticsFeedbackAirborne));
    else if (data.motionAvailable && data.skidding)
        status += L" · " + std::wstring(tr(Text::DlgDiagnosticsFeedbackSkidding));
    return status;
}

std::wstring feedbackMotionAge(const DiagnosticFeedbackTelemetry& data)
{
    return data.motionAvailable ? milliseconds(static_cast<double>(data.motionAgeMicros)) : unavailableValue;
}

const wchar_t* feedbackClipping(const DiagnosticFeedbackTelemetry& data)
{
    if (!data.requestAvailable)
        return unavailableValue;
    if (!data.clipped)
        return tr(Text::DlgDiagnosticsFeedbackClippingNone);
    return tr(data.impactClipped ? Text::DlgDiagnosticsFeedbackClippingImpact : Text::DlgDiagnosticsFeedbackClippingContinuous);
}

std::wstring feedbackSubmitted(const DiagnosticFeedbackTelemetry& data)
{
    return data.submittedAt ? precisePercentText(data.submitted) : unavailableValue;
}

bool hasFeedbackTelemetry(const DiagnosticFeedbackTelemetry& data)
{
    return data.motionAvailable || data.requestAvailable || data.submittedAt;
}

std::wstring feedbackTelemetry(const DiagnosticFeedbackTelemetry& data)
{
    if (!hasFeedbackTelemetry(data))
        return {};
    return std::wstring(tr(Text::DlgDiagnosticsFeedbackCalculatedShort)) + L" " +
        (data.requestAvailable ? precisePercentText(data.rawRequested) : unavailableValue) + L" · " +
        tr(Text::DlgDiagnosticsFeedbackLimitedShort) + L" " +
        (data.requestAvailable ? precisePercentText(data.requested) : unavailableValue) + L" · " +
        tr(Text::DlgDiagnosticsFeedbackSubmittedShort) + L" " + feedbackSubmitted(data);
}

std::wstring feedbackDetails(const DiagnosticFeedbackTelemetry& data)
{
    if (!hasFeedbackTelemetry(data))
        return {};
    return std::wstring(tr(Text::DlgDiagnosticsFeedbackClipping)) + L" — " + feedbackClipping(data) + L" · " +
        tr(Text::DlgDiagnosticsFeedbackMotionAge) + L" " + feedbackMotionAge(data);
}

std::wstring performanceTiming(const PerformanceTiming& timing)
{
    if (!timing.samples)
        return unavailableValue;
    return milliseconds(static_cast<double>(timing.totalMicros) / static_cast<double>(timing.samples)) +
        tr(Text::DlgDiagnosticsPerformanceMean) + L" · P95 " + milliseconds(static_cast<double>(timing.percentile(95))) +
        L" · P99 " + milliseconds(static_cast<double>(timing.percentile(99))) + L" · " +
        milliseconds(static_cast<double>(timing.maxMicros)) + tr(Text::DlgDiagnosticsPerformanceMaximum) +
        L" · " + std::to_wstring(timing.samples) +
        L" " + tr(timing.samples == 1 ? Text::DlgDiagnosticsPerformanceSample : Text::DlgDiagnosticsPerformanceSamples);
}

std::wstring clippedCount(const PerformanceReport& report)
{
    if (!report.forceSamples)
        return unavailableValue;
    const auto fraction = static_cast<float>(static_cast<double>(report.clippedSamples) /
                                              static_cast<double>(report.forceSamples));
    return std::to_wstring(report.clippedSamples) + L"/" + std::to_wstring(report.forceSamples) +
        L" (" + precisePercentText(fraction, false) + L")";
}

std::uint64_t continuousClippedSamples(const PerformanceReport& report)
{
    return report.clippedSamples - std::min(report.clippedSamples, report.impactClippedSamples);
}

std::wstring clippingSummary(const PerformanceReport& report)
{
    std::wstring text = std::wstring(tr(Text::DlgDiagnosticsFeedbackClipping)) + L" " + clippedCount(report);
    if (report.forceSamples) {
        text += L" · " + std::wstring(tr(Text::DlgDiagnosticsPerformanceContinuousShort)) + L" " +
            std::to_wstring(continuousClippedSamples(report)) + L" · " +
            tr(Text::DlgDiagnosticsPerformanceImpactShort) + L" " + std::to_wstring(report.impactClippedSamples);
    }
    return text;
}

bool recordingAvailable(const DialogState& state)
{
    return !state.gameRoot.empty() && !state.missingFiles && state.versionMismatch.empty();
}

bool canStopRecording(DiagnosticRecordingState state)
{
    return state == DiagnosticRecordingState::starting || state == DiagnosticRecordingState::recording ||
        state == DiagnosticRecordingState::waitingForGame || state == DiagnosticRecordingState::slotOccupied;
}

bool canExportRecording(const DiagnosticRecordingStatus& recording)
{
    std::error_code error;
    return (recording.state == DiagnosticRecordingState::complete || recording.state == DiagnosticRecordingState::failed) &&
        !recording.file.empty() && std::filesystem::is_regular_file(recording.file, error);
}

const wchar_t* recordingStateText(const DiagnosticRecordingStatus& recording, bool available)
{
    switch (recording.state) {
    case DiagnosticRecordingState::idle:
        return tr(available ? Text::DlgDiagnosticsPerformanceReady : Text::DlgDiagnosticsPerformanceUnavailable);
    case DiagnosticRecordingState::starting: return tr(Text::DlgDiagnosticsPerformanceStarting);
    case DiagnosticRecordingState::recording: return tr(Text::DlgDiagnosticsPerformanceActive);
    case DiagnosticRecordingState::waitingForGame: return tr(Text::DlgDiagnosticsRecordingWaitingForGame);
    case DiagnosticRecordingState::slotOccupied: return tr(Text::DlgDiagnosticsRecordingSlotOccupied);
    case DiagnosticRecordingState::stopping: return tr(Text::DlgDiagnosticsPerformanceStopping);
    case DiagnosticRecordingState::complete:
        return tr(recording.rows ? Text::DlgDiagnosticsPerformanceComplete : Text::DlgDiagnosticsPerformanceNoData);
    case DiagnosticRecordingState::failed:
        switch (recording.failure) {
        case DiagnosticRecordingFailure::file: return tr(Text::DlgDiagnosticsRecordingFileError);
        case DiagnosticRecordingFailure::unavailable: return tr(Text::DlgDiagnosticsRecordingUnavailable);
        case DiagnosticRecordingFailure::stopTimeout: return tr(Text::DlgDiagnosticsRecordingStopTimeout);
        case DiagnosticRecordingFailure::worker:
        case DiagnosticRecordingFailure::none:
            return tr(Text::DlgDiagnosticsPerformanceFailed);
        }
        break;
    }
    return unavailableValue;
}

std::wstring recordingSummary(const DiagnosticRecordingStatus& recording, bool available)
{
    std::wstring summary = recordingStateText(recording, available);
    if (recording.rows)
        summary += L" · " + std::wstring(tr(Text::DlgDiagnosticsRecordingRows)) + L" " + std::to_wstring(recording.rows);
    if (recording.dropped)
        summary += L" · " + std::wstring(tr(Text::DlgDiagnosticsRecordingDropped)) + L" " + std::to_wstring(recording.dropped);
    return summary;
}

const wchar_t* transmissionStatus(const DiagnosticTransmission& transmission)
{
    switch (transmission.shift) {
    case DiagnosticShiftState::automatic: return L"";
    case DiagnosticShiftState::ready: return tr(Text::DlgDiagnosticsTransmissionReady);
    case DiagnosticShiftState::unavailable: return tr(Text::DlgDiagnosticsTransmissionUnavailable);
    case DiagnosticShiftState::outsideRange: return tr(Text::DlgDiagnosticsTransmissionOutsideRange);
    case DiagnosticShiftState::stopToChangeDirection: return tr(Text::DlgDiagnosticsTransmissionStop);
    case DiagnosticShiftState::alreadyEngaged: return tr(Text::DlgDiagnosticsTransmissionEngaged);
    case DiagnosticShiftState::rejected: return tr(Text::DlgDiagnosticsTransmissionRejected);
    case DiagnosticShiftState::accepted: return tr(Text::DlgDiagnosticsTransmissionAccepted);
    case DiagnosticShiftState::neutralTransition: return tr(Text::DlgDiagnosticsTransmissionNeutral);
    case DiagnosticShiftState::expired: return tr(Text::DlgDiagnosticsTransmissionExpired);
    case DiagnosticShiftState::recalibrationNeeded: return tr(Text::DlgDiagnosticsTransmissionRecalibration);
    case DiagnosticShiftState::selectorUnavailable: return tr(Text::DlgDiagnosticsTransmissionSelector);
    case DiagnosticShiftState::selectorAmbiguous: return tr(Text::DlgDiagnosticsTransmissionAmbiguous);
    case DiagnosticShiftState::pending: return tr(Text::DlgDiagnosticsTransmissionPending);
    case DiagnosticShiftState::clutchRequired: return tr(Text::DlgDiagnosticsTransmissionClutchRequired);
    }
    return unavailableValue;
}

std::wstring transmissionSummary(const DiagnosticTransmission& transmission, DiagnosticMode mode)
{
    const auto gear = [](int position) -> std::wstring {
        if (position >= 1 && position <= 8)
            return std::to_wstring(position);
        if (position == 0)
            return L"R";
        return position == -1 ? L"N" : unavailableValue;
    };
    auto text = std::wstring(tr(transmission.manual ? Text::DlgDiagnosticsTransmissionManual
                                                   : Text::DlgDiagnosticsTransmissionAuto));
    if (mode == DiagnosticMode::usb)
        text += L" · " + std::wstring(tr(transmission.sequential ? Text::DlgDiagnosticsTransmissionSequential
                                                               : Text::DlgDiagnosticsTransmissionHPattern));
    if (transmission.hasRequestedMode && transmission.requestedManual != transmission.manual)
        text += L" · " + std::wstring(tr(Text::DlgDiagnosticsTransmissionModeRequested)) + L" " +
            tr(transmission.requestedManual ? Text::DlgDiagnosticsTransmissionManual : Text::DlgDiagnosticsTransmissionAuto);
    text += L" · " + std::wstring(tr(Text::DlgDiagnosticsTransmissionSelected)) + L" " + gear(transmission.selectedGear);
    text += L" · " + std::wstring(tr(Text::DlgDiagnosticsReportRequested)) + L" " + gear(transmission.requestedGear);
    text += L" · " + std::wstring(tr(Text::DlgDiagnosticsTransmissionActual)) + L" " + gear(transmission.engagedGear);
    return text;
}

std::wstring transmissionShortcut(const DiagnosticTransmission& transmission, std::uint64_t now)
{
    if (!isTransmissionHotkey(transmission.shortcutKey))
        return {};
    auto text = accessoryKeyLabel(transmission.shortcutKey);
    if (!transmission.shortcutAvailable)
        text += L" (" + std::wstring(tr(Text::DlgDiagnosticsTransmissionShortcutPaused)) + L")";
    text += L" · ";
    if (transmission.shortcutPressedAt) {
        text += tr(Text::DlgDiagnosticsTransmissionShortcutPressed);
        text += L" " + eventAge(transmission.shortcutPressedAt, now);
    } else {
        text += tr(Text::DlgDiagnosticsTransmissionShortcutNoPress);
    }
    return text;
}

std::wstring gearboxSampleAge(const DiagnosticTransmission& transmission, std::uint64_t sampledAt)
{
    if (!transmission.gearboxSampledAt || transmission.gearboxSampledAt > sampledAt)
        return unavailableValue;
    return std::to_wstring(sampledAt - transmission.gearboxSampledAt) + L" ms";
}

std::wstring clutchSummary(const DiagnosticClutch& clutch)
{
    auto text = std::wstring(tr(Text::DlgDiagnosticsReportRead)) + L" " +
        (clutch.inputAvailable ? percentText(clutch.input) : unavailableValue);
    const auto assistance = clutch.assistanceEnabled ? Text::DlgDiagnosticsClutchAssistanceOn
                                                    : Text::DlgDiagnosticsClutchAssistanceOff;
    text += L" · " + std::wstring(tr(assistance));
    if (clutch.probeActive || clutch.appliedAvailable) {
        text += L" · " + std::wstring(tr(Text::DlgDiagnosticsClutchApplied)) + L" " +
            (clutch.appliedAvailable ? percentText(clutch.applied) : unavailableValue);
        text += L" · " + std::wstring(tr(clutch.probeSupported ? Text::DlgDiagnosticsClutchProbeAvailable
                                                              : Text::DlgDiagnosticsClutchProbeUnavailable));
    }
    return text;
}

HRESULT pickCsvFile(HWND owner, std::filesystem::path& result)
{
    IFileSaveDialog* pointer{};
    auto status = CoCreateInstance(__uuidof(FileSaveDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pointer));
    if (FAILED(status))
        return status;
    const std::unique_ptr<IFileSaveDialog, decltype(releaseCom)> dialog(pointer);
    const COMDLG_FILTERSPEC filter{L"CSV (*.csv)", L"*.csv"};
    DWORD options{};
    if (FAILED(status = dialog->GetOptions(&options)) ||
        FAILED(status = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT)) ||
        FAILED(status = dialog->SetTitle(tr(Text::DlgDiagnosticsCsvTitle))) ||
        FAILED(status = dialog->SetFileTypes(1, &filter)) ||
        FAILED(status = dialog->SetDefaultExtension(L"csv")) ||
        FAILED(status = dialog->SetFileName(L"Chrome-Axis-driving.csv")) ||
        FAILED(status = dialog->Show(owner)))
        return status;

    IShellItem* selected{};
    if (FAILED(status = dialog->GetResult(&selected)))
        return status;
    const std::unique_ptr<IShellItem, decltype(releaseCom)> item(selected);
    PWSTR path{};
    if (FAILED(status = item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return status;
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedPath(path, CoTaskMemFree);
    result = ownedPath.get();
    return S_OK;
}

bool copyRecording(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    std::error_code ignored;
    if (std::filesystem::equivalent(source, destination, ignored))
        return true;

    const auto temporary = detail::stagedSettingsPath(destination);
    if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE)) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            DeleteFileW(temporary.c_str());
        return false;
    }
    if (detail::replaceSettingsFile(temporary, destination))
        return true;
    DeleteFileW(temporary.c_str());
    return false;
}

void exportDrivingCapture(HWND window, const DiagnosticRecordingStatus& recording)
{
    if (!canExportRecording(recording)) {
        MessageBoxW(window, tr(Text::DlgDiagnosticsCsvUnavailable), tr(Text::DlgDiagnosticsCsvTitle), MB_OK | MB_ICONWARNING);
        return;
    }
    std::filesystem::path destination;
    auto status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(status)) {
        status = pickCsvFile(window, destination);
        CoUninitialize();
    }
    if (status != HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
        (FAILED(status) || !copyRecording(recording.file, destination)))
        MessageBoxW(window, tr(Text::DlgDiagnosticsCsvError), tr(Text::DlgDiagnosticsCsvTitle), MB_OK | MB_ICONERROR);
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
        if (const auto shortcut = transmissionShortcut(read.snapshot.transmission, now); !shortcut.empty())
            line(Text::DlgDiagnosticsTransmissionShortcut, shortcut);
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
        if (snapshot.transmission.configured) {
            auto detail = transmissionSummary(snapshot.transmission, snapshot.mode);
            if (const auto status = transmissionStatus(snapshot.transmission); *status)
                detail += L" · " + std::wstring(status);
            line(Text::DlgDiagnosticsTransmission, detail);
            line(Text::DlgDiagnosticsTransmissionDataAge,
                 gearboxSampleAge(snapshot.transmission, read.lastDriveAt ? read.lastDriveAt : read.sampledAt));
        }
        if (read.lastDriveAt &&
            (snapshot.transmission.shortcutKey != read.snapshot.transmission.shortcutKey ||
             snapshot.transmission.shortcutPressedAt != read.snapshot.transmission.shortcutPressedAt)) {
            if (const auto shortcut = transmissionShortcut(snapshot.transmission, now); !shortcut.empty())
                line(Text::DlgDiagnosticsTransmissionShortcut, shortcut);
        }
        if (snapshot.clutch.configured)
            line(Text::DlgDiagnosticsClutch, clutchSummary(snapshot.clutch));
        line(Text::DlgDiagnosticsFeedback, feedbackStatus(snapshot));
        const auto& feedback = snapshot.feedbackData;
        if (hasFeedbackTelemetry(feedback)) {
            line(Text::DlgDiagnosticsFeedbackMotionAge, feedbackMotionAge(feedback));
            line(Text::DlgDiagnosticsFeedbackCalculated,
                 feedback.requestAvailable ? precisePercentText(feedback.rawRequested) : unavailableValue);
            line(Text::DlgDiagnosticsFeedbackLimited,
                 feedback.requestAvailable ? precisePercentText(feedback.requested) : unavailableValue);
            line(Text::DlgDiagnosticsFeedbackClipping, feedbackClipping(feedback));
            auto submitted = feedbackSubmitted(feedback);
            if (feedback.submittedAt)
                submitted += L" · " + eventAge(feedback.submittedAt, now);
            line(Text::DlgDiagnosticsFeedbackSubmitted, submitted);
        }
    }
    const auto accessories = accessoryView(read);
    if (!accessories.commands().empty()) {
        report += L"\r\n";
        line(Text::DlgDiagnosticsCommandHeading, accessoryViewContext(accessories, now));
        for (const auto& command : accessories.commands()) {
            report += L"- " + commandName(command) +
                L" · " + tr(Text::DlgDiagnosticsCommandInput) + L" " + commandSignal(command) +
                L" · " + tr(Text::DlgDiagnosticsCommandOutput) + L" " + commandOutput(command);
            if (!command.handbrakeCeilingPercent)
                report += L" · " + std::wstring(tr(Text::DlgDiagnosticsCommandLastSent)) + L" " + commandSent(command, now);
            report += L"\r\n";
        }
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
    const auto usbEvents = std::span(read.usbEvents).first(std::min<std::size_t>(read.usbEventCount, read.usbEvents.size()));
    if (!usbEvents.empty()) {
        report += L"\r\n" + std::wstring(tr(Text::DlgDiagnosticsUsbEvents)) + L"\r\n";
        for (const auto& event : usbEvents) {
            report += L"- " + eventAge(event.sampledAt, now) + L" — " + usbEventTitle(event) +
                usbEventDetails(event, state.devices) + L"\r\n";
        }
    }
    const auto recording = state.recorder.status();
    if (recording.state != DiagnosticRecordingState::idle)
        line(Text::DlgDiagnosticsRecordingStatus, recordingSummary(recording, recordingAvailable(state)));
    if (read.performance.state != PerformanceState::idle) {
        report += L"\r\n" + std::wstring(tr(Text::DlgDiagnosticsPerformanceReport)) + L"\r\n";
        const auto& performance = read.performance;
        const auto end = performance.state == PerformanceState::complete ? performance.endedAt : read.sampledAt;
        if (performance.startedAt && end >= performance.startedAt)
            line(Text::DlgDiagnosticsPerformanceDuration, std::to_wstring((end - performance.startedAt) / 1000) + L" s");
        for (const auto& control : performanceControls)
            line(control.label, performanceTiming(read.performance.*control.timing));
        line(Text::DlgDiagnosticsPerformanceExpired, std::to_wstring(read.performance.expiredReads));
        line(Text::DlgDiagnosticsPerformanceTransient, std::to_wstring(read.performance.transientReads));
        line(Text::DlgDiagnosticsPerformanceUsbOverruns, std::to_wstring(read.performance.usbIntervalOverruns));
        line(Text::DlgDiagnosticsPerformanceFeedbackOverruns, std::to_wstring(read.performance.feedbackIntervalOverruns));
        line(Text::DlgDiagnosticsPerformanceAccepted, std::to_wstring(read.performance.submittedEffects));
        const auto& capture = read.performance;
        line(Text::DlgDiagnosticsPerformanceClipped, clippedCount(capture));
        line(Text::DlgDiagnosticsPerformanceClippedContinuous,
             capture.forceSamples ? std::to_wstring(continuousClippedSamples(capture)) : unavailableValue);
        line(Text::DlgDiagnosticsPerformanceClippedImpact,
             capture.forceSamples ? std::to_wstring(capture.impactClippedSamples) : unavailableValue);
        line(Text::DlgDiagnosticsPerformancePeakRequested,
             capture.forceSamples ? precisePercentText(capture.peakRequestedForce, false) : unavailableValue);
        line(Text::DlgDiagnosticsPerformancePeakLimited,
             capture.forceSamples ? precisePercentText(capture.peakLimitedForce, false) : unavailableValue);
        line(Text::DlgDiagnosticsPerformancePeakSubmitted,
             capture.submittedEffects ? precisePercentText(capture.peakSubmittedForce, false) : unavailableValue);
        report += tr(Text::DlgDiagnosticsPerformanceReadingGuide);
        report += L"\r\n";
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

void refreshAccessoryDetails(HWND window, AccessoryDialogState& state)
{
    const auto now = GetTickCount64();
    const auto read = state.reader.read();
    const auto view = accessoryView(read);
    const auto commands = view.commands();
    setChangedText(window, IDC_DIAGNOSTICS_COMMAND_CONTEXT, accessoryViewContext(view, now));

    const auto list = GetDlgItem(window, IDC_DIAGNOSTICS_COMMAND_LIST);
    std::vector<std::wstring> labels;
    labels.reserve(commands.size());
    for (const auto& command : commands)
        labels.push_back(commandName(command) + L" · " + commandStateWithActivity(command));

    const auto selection = SendMessageW(list, LB_GETCURSEL, 0, 0);
    const auto selected = commands.empty() ? LB_ERR :
        static_cast<int>(std::clamp<LRESULT>(selection, 0, static_cast<LRESULT>(commands.size()) - 1));
    if (labels != state.rowLabels) {
        const auto top = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& label : labels)
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (selected != LB_ERR) {
            SendMessageW(list, LB_SETCURSEL, selected, 0);
            SendMessageW(list, LB_SETTOPINDEX, std::min(top, static_cast<LRESULT>(commands.size()) - 1), 0);
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        state.rowLabels = std::move(labels);
    }

    const auto detail = [window](int id, Text label, std::wstring_view value) {
        setChangedText(window, id, std::wstring(tr(label)) + L" — " + std::wstring(value));
    };
    if (selected == LB_ERR) {
        for (const auto id : {IDC_DIAGNOSTICS_COMMAND_INPUT, IDC_DIAGNOSTICS_COMMAND_OUTPUT, IDC_DIAGNOSTICS_COMMAND_SENT})
            setChangedText(window, id, L"");
        return;
    }
    const auto& command = commands[static_cast<std::size_t>(selected)];
    detail(IDC_DIAGNOSTICS_COMMAND_INPUT, Text::DlgDiagnosticsCommandInput, commandSignal(command));
    detail(IDC_DIAGNOSTICS_COMMAND_OUTPUT, Text::DlgDiagnosticsCommandOutput, commandOutput(command));
    if (command.handbrakeCeilingPercent)
        setChangedText(window, IDC_DIAGNOSTICS_COMMAND_SENT, L"");
    else
        detail(IDC_DIAGNOSTICS_COMMAND_SENT, Text::DlgDiagnosticsCommandLastSent, commandSent(command, now));
}

INT_PTR CALLBACK accessoryDetailsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    const auto state = reinterpret_cast<AccessoryDialogState*>(GetWindowLongPtrW(window, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        SetWindowTextW(window, tr(Text::DlgDiagnosticsCommandTitle));
        SetDlgItemTextW(window, IDC_MODAL_LIST_HEADING, tr(Text::DlgDiagnosticsCommandList));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgDiagnosticsClose));
        DialogTheme::attach(window, tr(Text::DlgDiagnosticsCommandHeading), IDC_DIAGNOSTICS_COMMAND_CONTEXT,
                            IDC_DIAGNOSTICS_COMMAND_LIST, {IDCANCEL});
        refreshAccessoryDetails(window, *reinterpret_cast<AccessoryDialogState*>(lParam));
        SetTimer(window, refreshTimer, refreshIntervalMs, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wParam == refreshTimer && state) {
            refreshAccessoryDetails(window, *state);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_DIAGNOSTICS_COMMAND_LIST && HIWORD(wParam) == LBN_SELCHANGE && state) {
            refreshAccessoryDetails(window, *state);
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
        break;
    }
    return FALSE;
}

void refreshPerformance(HWND window, const DialogState& state, const DiagnosticRead& read)
{
    const auto recording = state.recorder.status();
    const bool canStop = canStopRecording(recording.state);
    const bool stopping = recording.state == DiagnosticRecordingState::stopping;
    const bool available = recordingAvailable(state);
    const auto& performance = read.performance;
    const bool showResults = performance.state != PerformanceState::idle;
    EnableWindow(GetDlgItem(window, IDC_DIAGNOSTICS_SAVE_CSV), canExportRecording(recording));
    setChangedText(window, IDC_DIAGNOSTICS_PERFORMANCE,
                   tr(canStop || stopping ? Text::DlgDiagnosticsPerformanceStop : Text::DlgDiagnosticsPerformanceStart));
    EnableWindow(GetDlgItem(window, IDC_DIAGNOSTICS_PERFORMANCE), !stopping && (canStop || available));
    setChangedText(window, IDC_DIAGNOSTICS_PERFORMANCE_STATE, recordingSummary(recording, available));
    for (const auto& control : performanceControls)
        setChangedText(window, control.valueId, performanceTiming(performance.*control.timing));
    setChangedText(window, IDC_DIAGNOSTICS_PERFORMANCE_WARNINGS, showResults
        ? std::wstring(tr(Text::DlgDiagnosticsPerformanceExpired)) + L" · " + std::to_wstring(performance.expiredReads) +
            L"    " + tr(Text::DlgDiagnosticsPerformanceTransient) + L" · " + std::to_wstring(performance.transientReads)
        : L"");
    setChangedText(window, IDC_DIAGNOSTICS_PERFORMANCE_OVERRUNS, showResults
        ? std::wstring(tr(Text::DlgDiagnosticsPerformanceUsbOverruns)) + L" · " +
            std::to_wstring(performance.usbIntervalOverruns) + L"    " +
            tr(Text::DlgDiagnosticsPerformanceFeedbackOverruns) + L" · " +
            std::to_wstring(performance.feedbackIntervalOverruns)
        : L"");
    setChangedText(window, IDC_DIAGNOSTICS_PERFORMANCE_FORCE_COUNTS, showResults
        ? std::wstring(tr(Text::DlgDiagnosticsPerformanceAccepted)) + L" · " +
            std::to_wstring(performance.submittedEffects)
        : L"");
    setChangedText(window, IDC_DIAGNOSTICS_PERFORMANCE_CLIPPING, showResults ? clippingSummary(performance) : L"");
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
    if (!state.missingFiles && state.versionMismatch.empty()) {
        if (read.usbEventCount) {
            const auto& latest = read.usbEvents[read.usbEventCount - 1];
            auto detail = std::wstring(tr(Text::DlgDiagnosticsLatestUsb)) +
                L" · " + usbEventTitle(latest);
            if (slowFeedbackEvent(latest)) {
                detail += usbEventDetails(latest, state.devices);
            } else {
                detail += L" · " + eventAge(latest.sampledAt, now);
            }
            if (latest.kind == DiagnosticUsbEventKind::commandsWaiting &&
                (latest.accessories == DiagnosticAccessoryState::modifierBlocked ||
                 latest.accessories == DiagnosticAccessoryState::selectorAmbiguous)) {
                detail += L" · " + std::wstring(accessoryText(latest.accessories));
            } else if (latest.kind == DiagnosticUsbEventKind::commandsWaiting && latest.blockerCount) {
                detail += L" · " + usbBlockerName(latest.blockers.front());
                if (latest.blockerCount > 1)
                    detail += L" +" + std::to_wstring(latest.blockerCount - 1);
            }
            setChangedText(window, IDC_MODAL_INTRO, detail);
        } else {
            setChangedText(window, IDC_MODAL_INTRO, tr(Text::DlgDiagnosticsIntro));
        }
    }
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
    const bool showTransmission = showControls && snapshot.transmission.configured;
    const auto& shortcutSource = connected && isTransmissionHotkey(read.snapshot.transmission.shortcutKey)
        ? read.snapshot.transmission : snapshot.transmission;
    const auto shortcut = showControls ? transmissionShortcut(shortcutSource, now) : std::wstring{};
    auto summary = showTransmission ? transmissionSummary(snapshot.transmission, snapshot.mode) : std::wstring{};
    if (showTransmission) {
        summary += L" · " + std::wstring(tr(Text::DlgDiagnosticsTransmissionData)) + L" " +
            gearboxSampleAge(snapshot.transmission, showLastDrive ? read.lastDriveAt : read.sampledAt);
    }
    auto transmissionDetails = showTransmission ? std::wstring(transmissionStatus(snapshot.transmission)) : std::wstring{};
    if (!shortcut.empty()) {
        if (!transmissionDetails.empty())
            transmissionDetails = shortcut + L" · " + transmissionDetails;
        else
            transmissionDetails = shortcut;
    }
    setChangedText(window, IDC_DIAGNOSTICS_TRANSMISSION_LABEL,
                   showTransmission || !shortcut.empty() ? tr(Text::DlgDiagnosticsTransmission) : L"");
    setChangedText(window, IDC_DIAGNOSTICS_TRANSMISSION, showTransmission ? summary : shortcut);
    setChangedText(window, IDC_DIAGNOSTICS_TRANSMISSION_STATUS, showTransmission ? transmissionDetails : L"");
    const bool showClutch = showControls && snapshot.clutch.configured;
    setChangedText(window, IDC_DIAGNOSTICS_CLUTCH_LABEL, showClutch ? tr(Text::DlgDiagnosticsClutch) : L"");
    setChangedText(window, IDC_DIAGNOSTICS_CLUTCH, showClutch ? clutchSummary(snapshot.clutch) : L"");
    setChangedText(window, IDC_DIAGNOSTICS_FEEDBACK, showControls ? feedbackStatus(snapshot) : unavailableValue);
    setChangedText(window, IDC_DIAGNOSTICS_FORCE_TELEMETRY, showControls ? feedbackTelemetry(snapshot.feedbackData) : L"");
    setChangedText(window, IDC_DIAGNOSTICS_FORCE_DETAILS, showControls ? feedbackDetails(snapshot.feedbackData) : L"");
    refreshPerformance(window, state, read);
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
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_ACCESSORY_DETAILS, tr(Text::DlgDiagnosticsAccessoryDetails));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_FEEDBACK_LABEL, tr(Text::DlgDiagnosticsFeedback));
        for (const auto& control : performanceControls)
            SetDlgItemTextW(window, control.labelId, tr(control.label));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_COPY, tr(Text::DlgDiagnosticsCopy));
        SetDlgItemTextW(window, IDC_DIAGNOSTICS_SAVE_CSV, tr(Text::DlgDiagnosticsCsvSave));
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
        if (LOWORD(wParam) == IDC_DIAGNOSTICS_ACCESSORY_DETAILS && state) {
            AccessoryDialogState details{state->reader, {}};
            const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DIAGNOSTICS_ACCESSORIES),
                                               window, accessoryDetailsProc, reinterpret_cast<LPARAM>(&details));
            if (result == -1)
                MessageBoxW(window, tr(Text::DlgDiagnosticsOpenError), tr(Text::DlgDiagnosticsCommandTitle), MB_OK | MB_ICONERROR);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_DIAGNOSTICS_COPY && state) {
            const auto read = state->reader.read();
            const bool copied = copyToClipboard(window, diagnosticReport(read, *state, GetTickCount64()));
            if (SetTimer(window, copyTimer, copyFeedbackMs, nullptr))
                SetDlgItemTextW(window, IDC_DIAGNOSTICS_COPY,
                                tr(copied ? Text::DlgDiagnosticsCopied : Text::DlgDiagnosticsCopyError));
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_DIAGNOSTICS_SAVE_CSV && state) {
            exportDrivingCapture(window, state->recorder.status());
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_DIAGNOSTICS_PERFORMANCE && state) {
            const auto status = state->recorder.status().state;
            if (canStopRecording(status))
                state->recorder.stop();
            else if (status != DiagnosticRecordingState::stopping && recordingAvailable(*state))
                state->recorder.start();
            refresh(window, *state);
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

void showDiagnostics(HWND owner, const std::filesystem::path& profileRoot, std::span<const DeviceInfo> devices,
                     DiagnosticReader& reader, DiagnosticRecorder& recorder)
{
    const auto pluginDirectory = profileRoot.parent_path();
    const auto gameRoot = installedGameRoot(pluginDirectory);
    DialogState state{reader, recorder, gameRoot, profileRoot, detectedModels(devices), devices};
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
