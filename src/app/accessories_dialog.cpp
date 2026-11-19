#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "app_input.hpp"
#include "device_inventory.hpp"
#include "dialog_theme.hpp"
#include "profile_editor.hpp"
#include "ui_theme.hpp"

#include "chrome_axis/accessories.hpp"
#include "chrome_axis/calibration.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/keyboard_dialog.hpp"
#include "chrome_axis/presets.hpp"
#include "chrome_axis/shifter.hpp"

#include <dbt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <system_error>
#include <utility>

namespace chrome_axis {

std::wstring accessoryKeyLabel(unsigned int key)
{
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return tr(Text::DlgKeyNumpad) + std::to_wstring(key - VK_NUMPAD0);
    if (key >= VK_F1 && key <= VK_F12)
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    switch (key) {
    case VK_SPACE: return tr(Text::DlgKeySpace);
    case VK_UP: return tr(Text::DlgKeyArrowUp);
    case VK_DOWN: return tr(Text::DlgKeyArrowDown);
    case VK_LEFT: return tr(Text::DlgKeyArrowLeft);
    case VK_RIGHT: return tr(Text::DlgKeyArrowRight);
    case VK_ADD: return tr(Text::DlgKeyNumpadAdd);
    case VK_SUBTRACT: return tr(Text::DlgKeyNumpadSubtract);
    default: return std::wstring(1, static_cast<wchar_t>(key));
    }
}

BindingKeyEdit chooseBindingKey(HWND owner, AccessoryProfile& bindings, std::size_t row)
{
    auto& binding = bindings[row];
    if (binding.handbrakeCeilingPercent || binding.clutchMode || isNativeTransmissionBinding(binding))
        return BindingKeyEdit::unchanged;
    const auto key = chooseAccessoryKey(owner, binding.key);
    if (!key || *key == binding.key)
        return BindingKeyEdit::unchanged;
    if (std::ranges::find(bindings, *key, &AccessoryBinding::key) != bindings.end())
        return BindingKeyEdit::occupied;
    if (*key == VK_SPACE && std::ranges::any_of(bindings, [&](const auto& other) {
            return other.handbrakeCeilingPercent && sameShifterInput(binding, other);
        }))
        return BindingKeyEdit::handbrakeInputConflict;
    binding.key = *key;
    return BindingKeyEdit::changed;
}

namespace {

constexpr UINT_PTR captureTimer = 1;
constexpr UINT captureIntervalMs = 30;
constexpr ULONGLONG captureTimeoutMs = 12000;
constexpr UINT_PTR previewTimer = 2;
constexpr UINT previewIntervalMs = 50;
constexpr UINT_PTR inventoryTimer = 3;
constexpr UINT inventoryWatchMs = 80;

std::wstring percent(int value)
{
    return std::to_wstring(value) + tr(Text::mainPercentSuffix);
}

enum class CaptureStage { Idle, Digital, AxisRest, AwaitingTravel, AxisTravel };

struct ClutchOption {
    ClutchMode mode;
    Text label;
    Text hint;
};

constexpr std::array clutchOptions{
    ClutchOption{ClutchMode::off, Text::DlgClutchOff, Text::DlgClutchOffHint},
    ClutchOption{ClutchMode::assist, Text::DlgClutchAssist, Text::DlgClutchAssistHint},
    ClutchOption{ClutchMode::experiment, Text::DlgClutchExperiment, Text::DlgClutchExperimentHint},
};

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const auto bytes = static_cast<int>(text.size());
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, result.data(), length);
    return result;
}

} // namespace

std::wstring accessoryInputLabel(const AccessoryBinding& binding)
{
    constexpr std::array directions{Text::DlgPovNorth, Text::DlgPovNorthEast, Text::DlgPovEast,
        Text::DlgPovSouthEast, Text::DlgPovSouth, Text::DlgPovSouthWest, Text::DlgPovWest, Text::DlgPovNorthWest};
    switch (binding.kind) {
    case AccessoryKind::Button:
        return tr(Text::DlgInputButton) + std::to_wstring(binding.index + 1);
    case AccessoryKind::Pov:
        return tr(Text::DlgInputDpad) + std::to_wstring(binding.index + 1) + L" · "
            + tr(directions[binding.engaged / povDirectionStep]);
    case AccessoryKind::Axis:
        return tr(Text::DlgInputAxis) + wide(axisName(binding.index)) + (binding.engaged > binding.rest ? L" (+)" : L" (−)");
    }
    return {};
}

std::wstring shifterRoleLabel(ShifterRole role)
{
    switch (role) {
    case ShifterRole::None: return {};
    case ShifterRole::Reverse: return tr(Text::DlgShifterReverse);
    case ShifterRole::Upshift: return tr(Text::DlgShifterUpshift);
    case ShifterRole::Downshift: return tr(Text::DlgShifterDownshift);
    default:
        const auto gear = static_cast<int>(role) - static_cast<int>(ShifterRole::Gear1) + 1;
        return tr(Text::DlgShifterGear) + std::to_wstring(gear);
    }
}

std::wstring accessoryBindingInputLabel(const AccessoryBinding& binding)
{
    auto input = accessoryInputLabel(binding);
    if (binding.role != ShifterRole::None)
        input = shifterRoleLabel(binding.role) + L" · " + input;
    return input;
}

std::wstring accessoryBindingOutputLabel(const AccessoryBinding& binding, bool nativeVerified)
{
    if (binding.nativeSequential)
        return tr(Text::DlgShifterNativeSequentialOutput);
    if (!binding.nativeShifter)
        return accessoryKeyLabel(binding.key);
    if (binding.role == ShifterRole::Gear8)
        return tr(Text::DlgShifterNativeEighthInactive);
    return tr(nativeVerified ? Text::DlgShifterNativeOutput : Text::DlgShifterNativeUnverifiedOutput);
}

const wchar_t* accessoryActivationLabel(AccessoryActivation activation)
{
    return tr(activation == AccessoryActivation::pulse ? Text::DlgAccessoryActivationPulse : Text::DlgAccessoryActivationHold);
}

void populateAccessoryActivation(HWND window, int control)
{
    for (const auto activation : {AccessoryActivation::hold, AccessoryActivation::pulse}) {
        SendDlgItemMessageW(window, control, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(accessoryActivationLabel(activation)));
    }
}

namespace {

unsigned int readPercent(HWND window, int control)
{
    wchar_t text[5]{};
    const auto length = GetDlgItemTextW(window, control, text, static_cast<int>(std::size(text)));
    if (!length || length > 3)
        return 0;
    unsigned int value = 0;
    for (UINT i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9')
            return 0;
        value = value * 10 + static_cast<unsigned int>(text[i] - L'0');
    }
    return value;
}

std::optional<AccessoryThresholds> readThresholds(HWND window)
{
    const AccessoryThresholds values{
        readPercent(window, IDC_THRESHOLDS_PRESS), readPercent(window, IDC_THRESHOLDS_RELEASE)};
    if (!validAccessoryThresholds(values))
        return std::nullopt;
    return values;
}

void setThresholds(HWND window, AccessoryThresholds values)
{
    SetDlgItemInt(window, IDC_THRESHOLDS_PRESS, values.press, FALSE);
    SetDlgItemInt(window, IDC_THRESHOLDS_RELEASE, values.release, FALSE);
}

void updateThresholdsActions(HWND window)
{
    const bool valid = readThresholds(window).has_value();
    EnableWindow(GetDlgItem(window, IDOK), valid);
    SetDlgItemTextW(window, IDC_THRESHOLDS_STATUS, valid ? L"" : tr(Text::DlgThresholdsInvalid));
}

INT_PTR CALLBACK thresholdsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        SetWindowTextW(window, tr(Text::DlgThresholdsTitle));
        SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(Text::DlgThresholdsHint));
        SetDlgItemTextW(window, IDC_THRESHOLDS_PRESS_LABEL, tr(Text::DlgThresholdsPress));
        SetDlgItemTextW(window, IDC_THRESHOLDS_RELEASE_LABEL, tr(Text::DlgThresholdsRelease));
        SetDlgItemTextW(window, IDC_THRESHOLDS_DEFAULTS, tr(Text::DlgThresholdsDefaults));
        SetDlgItemTextW(window, IDOK, tr(Text::DlgThresholdsApply));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, tr(Text::DlgThresholdsHeading), IDC_THRESHOLDS_STATUS, 0, {IDOK});
        for (const int control : {IDC_THRESHOLDS_PRESS, IDC_THRESHOLDS_RELEASE})
            SendDlgItemMessageW(window, control, EM_SETLIMITTEXT, 3, 0);
        setThresholds(window, *reinterpret_cast<const AccessoryThresholds*>(lParam));
        updateThresholdsActions(window);
        return TRUE;
    }
    if (message != WM_COMMAND)
        return FALSE;
    switch (LOWORD(wParam)) {
    case IDC_THRESHOLDS_PRESS:
    case IDC_THRESHOLDS_RELEASE:
        if (HIWORD(wParam) == EN_CHANGE)
            updateThresholdsActions(window);
        return TRUE;
    case IDC_THRESHOLDS_DEFAULTS:
        setThresholds(window, {});
        return TRUE;
    case IDOK:
        if (const auto values = readThresholds(window)) {
            auto* selected = reinterpret_cast<AccessoryThresholds*>(GetWindowLongPtrW(window, DWLP_USER));
            *selected = *values;
            EndDialog(window, IDOK);
        }
        return TRUE;
    case IDCANCEL:
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool isLegacyHandbrakeAxis(const AccessoryBinding& binding)
{
    return binding.kind == AccessoryKind::Axis && binding.role == ShifterRole::None && binding.key == VK_SPACE;
}

AccessoryBinding asHandbrake(AccessoryBinding binding, unsigned int ceiling)
{
    binding.key = 0;
    binding.thresholds = {};
    binding.activation = AccessoryActivation::hold;
    binding.handbrakeCeilingPercent = ceiling;
    return binding;
}

std::optional<AccessoryProfile> readAccessorySettings(HWND owner, const std::filesystem::path& path)
{
    std::string error;
    auto saved = loadEditorAccessories(path, error);
    if (!saved) {
        const auto message = tr(Text::DlgAccessoriesReadError) + localizedError(error);
        MessageBoxW(owner, message.c_str(), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONERROR);
    }
    return saved;
}

bool saveAccessorySettings(HWND owner, const std::filesystem::path& root, const ProfilePreset& preset,
                           const std::optional<ProfileFileRevision>& revision, const AccessoryProfile& bindings)
{
    if (isTemporaryProfileDirectory(preset.directory)) {
        MessageBoxW(owner, tr(Text::DlgAccessoriesExtractBeforeSaving), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONWARNING);
        return false;
    }
    const auto save = saveEditorAccessories(root, preset, revision, bindings);
    if (save.result == PresetSaveResult::saved)
        return true;

    const auto reason = save.result == PresetSaveResult::changed ? Text::DlgAccessoriesChanged
        : save.reason == ProfileEditorError::calibrationUnreadable ? Text::mainSaveCalibrationUnreadable
        : save.reason == ProfileEditorError::accessoriesUnreadable ? Text::DlgAccessoriesChanged
        : save.reason == ProfileEditorError::axisConflict ? Text::DlgAccessoryDriveAxisConflict
        : save.reason == ProfileEditorError::dedicatedAccessoryAxisConflict ? Text::DlgDedicatedAccessoryAxisConflict
        : save.reason == ProfileEditorError::createDirectoryFailed ? Text::DlgAccessoriesCreateProfileFailed
        : Text::DlgAccessoriesSaveError;
    auto message = std::wstring(tr(reason));
    if (reason == Text::DlgAccessoriesSaveError && !save.details.empty())
        message += localizedError(save.details);
    MessageBoxW(owner, message.c_str(), tr(Text::DlgAccessoriesTitle), MB_OK |
        (reason == Text::DlgAccessoriesSaveError || reason == Text::DlgAccessoriesCreateProfileFailed
            ? MB_ICONERROR : MB_ICONWARNING));
    return false;
}

class AccessoryDialog {
public:
    AccessoryDialog(AppInput& input, DeviceInventory& inventory, const std::filesystem::path& root,
                    const ProfilePreset& preset, AccessorySection section)
        : input_(input), inventory_(inventory), root_(root), preset_(preset),
          path_(preset.directory / accessoryFilename), section_(section) {}

    bool initialise(HWND window);
    Text heading() const noexcept
    {
        return sectionText(Text::DlgAccessoriesHeading, Text::DlgHandbrakeHeading, Text::DlgClutchHeading);
    }
    void capture();
    void sample();
    void refreshDevices();
    void deviceTopologyChanged();
    void refreshIfIdle();
    void clearCapture();
    void refreshActions();
    void selectPreview();
    void activationChanged(bool active);
    void pollPreview();
    void drawPreview(const DRAWITEMSTRUCT& item) const;
    void selectCapturedInput();
    void chooseKey();
    void changeActivation();
    void add();
    void edit();
    void editThresholds();
    void useCapturedAnalog();
    void useLegacyHandbrake();
    void changeHandbrakeCeiling();
    void removeAnalog();
    void changeClutchMode();
    void remove();
    void save();
    void cancel();

private:
    LRESULT send(int control, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) const
    {
        return SendDlgItemMessageW(window_, control, message, wParam, lParam);
    }

    bool handbrakePage() const noexcept { return section_ == AccessorySection::handbrake; }
    bool clutchPage() const noexcept { return section_ == AccessorySection::clutch; }
    bool analogPage() const noexcept { return section_ != AccessorySection::keys; }
    Text sectionText(Text keys, Text handbrake, Text clutch) const noexcept
    {
        return clutchPage() ? clutch : handbrakePage() ? handbrake : keys;
    }
    void configureSection();
    const DeviceInfo* selectedDevice() const;
    std::optional<std::size_t> selectedBinding() const;
    std::optional<std::size_t> selectedCapture() const;
    std::optional<std::size_t> handbrakeBinding() const;
    std::optional<std::size_t> clutchBinding() const;
    std::optional<std::size_t> analogBinding() const;
    std::optional<std::size_t> legacyHandbrakeBinding() const;
    std::wstring deviceLabel(std::string_view id) const;
    void selectDevice(std::string_view id);
    void rememberSelectedDevice();
    void restoreDeviceSelection();
    void setStatus(const wchar_t* text) const;
    void startSampling(CaptureStage stage, const wchar_t* text);
    bool rejectDisconnectedRest(const DeviceInfo& device);
    void interruptCapture();
    void setPreviewFocus(bool focused);
    void finishCapture();
    void refreshBindings(std::optional<std::size_t> selection = {});
    void refreshAnalogActions(bool idle, bool captured);
    void applyHandbrake(AccessoryBinding binding);
    void applyClutch(AccessoryBinding binding);
    void renderPreview(std::wstring status, std::optional<int> travel, std::optional<int> handbrake = {});
    void setHandbrakePreview(std::optional<int> tenths) const;

    AppInput& input_;
    DeviceInventory& inventory_;
    const std::filesystem::path& root_;
    const ProfilePreset& preset_;
    std::filesystem::path path_;
    std::optional<ProfileFileRevision> revision_;
    HWND window_{};
    std::vector<DeviceInfo> devices_;
    AccessoryProfile bindings_;
    std::optional<Profile> savedAxes_;
    std::vector<AccessoryBinding> captured_;
    std::optional<AccessoryBinding> previewBinding_;
    std::optional<AccessoryReader> preview_;
    std::wstring previewStatus_;
    std::optional<int> previewTravel_;
    std::optional<int> previewHandbrake_;
    std::optional<DeviceSample> previous_;
    PovCapture povCapture_;
    std::optional<AxisCapture> rest_;
    CalibrationCapture measurements_;
    CaptureStage stage_{CaptureStage::Idle};
    const AccessorySection section_;
    std::string deviceId_;
    ClutchMode clutchMode_{ClutchMode::off};
    unsigned int key_{};
    unsigned int handbrakeCeiling_{defaultHandbrakeCeilingPercent};
    ULONGLONG deadline_{};
    DeviceReadClock::time_point travelStartedAt_{};
    bool modified_{};
    bool validCeiling_{true};
    bool previewPolling_{};
    bool previewAxis_{};
    bool captureReadInterrupted_{};
    bool captureHadSignal_{};
    std::optional<std::uint64_t> inventoryRevision_;
};

bool AccessoryDialog::initialise(HWND window)
{
    window_ = window;
    revision_ = profileFileRevision(path_);
    auto saved = readAccessorySettings(window_, path_);
    if (!saved) {
        EndDialog(window_, IDCANCEL);
        return false;
    }
    bindings_ = std::move(*saved);
    send(IDC_ACCESSORY_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(Text::DlgDigital)));
    send(IDC_ACCESSORY_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(Text::DlgAnalog)));
    send(IDC_ACCESSORY_TYPE, CB_SETCURSEL, 0);
    populateAccessoryActivation(window_, IDC_ACCESSORY_ACTIVATION);
    if (const auto binding = handbrakeBinding())
        handbrakeCeiling_ = bindings_[*binding].handbrakeCeilingPercent;
    if (const auto binding = clutchBinding())
        clutchMode_ = *bindings_[*binding].clutchMode;
    for (const auto& option : clutchOptions)
        send(IDC_CLUTCH_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(option.label)));
    const auto mode = std::ranges::find(clutchOptions, clutchMode_, &ClutchOption::mode);
    send(IDC_CLUTCH_MODE, CB_SETCURSEL, static_cast<WPARAM>(mode - clutchOptions.begin()));
    send(IDC_HANDBRAKE_CEILING, EM_SETLIMITTEXT, 3);
    SetDlgItemInt(window_, IDC_HANDBRAKE_CEILING, handbrakeCeiling_, FALSE);
    if (analogPage()) {
        std::string error;
        savedAxes_ = loadProfile(preset_.directory / profileFilename, error);
    }
    configureSection();
    SetTimer(window_, inventoryTimer, inventoryWatchMs, nullptr);
    refreshBindings();
    refreshIfIdle();
    if (!inventory_.current())
        setStatus(tr(Text::mainScanningDevices));
    SetFocus(GetDlgItem(window_, selectedDevice() ? IDC_ACCESSORY_DEVICE : IDC_ACCESSORY_REFRESH));
    return false;
}

const DeviceInfo* AccessoryDialog::selectedDevice() const
{
    const auto selected = send(IDC_ACCESSORY_DEVICE, CB_GETCURSEL);
    return selected >= 0 && static_cast<std::size_t>(selected) < devices_.size()
        ? &devices_[static_cast<std::size_t>(selected)] : nullptr;
}

std::optional<std::size_t> AccessoryDialog::selectedBinding() const
{
    if (analogPage())
        return {};
    const auto row = send(IDC_ACCESSORY_LIST, LB_GETCURSEL);
    if (row == LB_ERR)
        return {};
    const auto index = send(IDC_ACCESSORY_LIST, LB_GETITEMDATA, static_cast<WPARAM>(row));
    if (index < 0 || static_cast<std::size_t>(index) >= bindings_.size())
        return {};
    return static_cast<std::size_t>(index);
}

std::optional<std::size_t> AccessoryDialog::selectedCapture() const
{
    const auto index = send(IDC_ACCESSORY_INPUT, CB_GETCURSEL);
    if (index < 0 || static_cast<std::size_t>(index) >= captured_.size())
        return {};
    return static_cast<std::size_t>(index);
}

std::optional<std::size_t> AccessoryDialog::handbrakeBinding() const
{
    const auto found = std::ranges::find_if(bindings_, [](const auto& binding) {
        return binding.handbrakeCeilingPercent != 0;
    });
    if (found == bindings_.end())
        return {};
    return static_cast<std::size_t>(found - bindings_.begin());
}

std::optional<std::size_t> AccessoryDialog::clutchBinding() const
{
    const auto found = std::ranges::find_if(bindings_, [](const auto& binding) { return binding.clutchMode.has_value(); });
    if (found == bindings_.end())
        return {};
    return static_cast<std::size_t>(found - bindings_.begin());
}

std::optional<std::size_t> AccessoryDialog::analogBinding() const
{
    return clutchPage() ? clutchBinding() : handbrakeBinding();
}

std::optional<std::size_t> AccessoryDialog::legacyHandbrakeBinding() const
{
    const auto found = std::ranges::find_if(bindings_, isLegacyHandbrakeAxis);
    if (found == bindings_.end())
        return {};
    return static_cast<std::size_t>(found - bindings_.begin());
}

std::wstring AccessoryDialog::deviceLabel(std::string_view id) const
{
    const auto device = std::ranges::find(devices_, id, &DeviceInfo::id);
    return wide(device == devices_.end() || device->name.empty() ? id : std::string_view(device->name));
}

void AccessoryDialog::selectDevice(std::string_view id)
{
    const auto selection = inventoryDeviceSelection(devices_, id, !analogPage() || devices_.size() == 1);
    send(IDC_ACCESSORY_DEVICE, CB_SETCURSEL, selection ? static_cast<WPARAM>(*selection) : static_cast<WPARAM>(CB_ERR));
}

void AccessoryDialog::rememberSelectedDevice()
{
    if (const auto* device = selectedDevice())
        deviceId_ = device->id;
}

void AccessoryDialog::restoreDeviceSelection()
{
    if (!analogPage() || !deviceId_.empty()) {
        selectDevice(deviceId_);
        return;
    }

    auto axis = analogBinding();
    if (!axis && handbrakePage())
        axis = legacyHandbrakeBinding();
    selectDevice(axis ? std::string_view(bindings_[*axis].deviceId) : std::string_view{});
}

void AccessoryDialog::configureSection()
{
    const auto show = [&](int control, bool visible) {
        ShowWindow(GetDlgItem(window_, control), visible ? SW_SHOW : SW_HIDE);
    };
    for (const auto control : {IDC_ACCESSORY_TYPE_LABEL, IDC_ACCESSORY_TYPE,
                               IDC_MODAL_KEY_LABEL, IDC_ACCESSORY_KEY, IDC_ACCESSORY_ADD,
                               IDC_MODAL_LIST_HEADING, IDC_ACCESSORY_ACTIVATION_LABEL,
                               IDC_ACCESSORY_ACTIVATION, IDC_ACCESSORY_THRESHOLDS, IDC_ACCESSORY_LIST,
                               IDC_ACCESSORY_EDIT, IDC_ACCESSORY_REMOVE})
        show(control, !analogPage());
    for (const auto control : {IDC_HANDBRAKE_AXIS, IDC_HANDBRAKE_APPLY_AXIS,
                               IDC_HANDBRAKE_REMOVE, IDC_HANDBRAKE_HEADING})
        show(control, analogPage());
    for (const auto control : {IDC_HANDBRAKE_USE_SPACE, IDC_HANDBRAKE_CEILING_LABEL,
                               IDC_HANDBRAKE_CEILING, IDC_HANDBRAKE_STATUS})
        show(control, handbrakePage());
    for (const auto control : {IDC_CLUTCH_MODE, IDC_CLUTCH_MODE_LABEL, IDC_CLUTCH_STATUS})
        show(control, clutchPage());
    SetDlgItemTextW(window_, IDC_MODAL_INTRO,
        tr(sectionText(Text::DlgAccessoriesIntro, Text::DlgHandbrakeIntro, Text::DlgClutchIntro)));
    SetDlgItemTextW(window_, IDC_ACCESSORY_INPUT_LABEL,
        tr(analogPage() ? Text::DlgHandbrakeDetectedAxis : Text::DlgAccessoriesCapturedInput));
    SetDlgItemTextW(window_, IDC_HANDBRAKE_HEADING, tr(Text::DlgAccessorySavedAxis));
    SetDlgItemTextW(window_, IDC_HANDBRAKE_REMOVE, tr(clutchPage() ? Text::DlgClutchRemove : Text::DlgHandbrakeRemove));
    SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_HEADING,
        tr(sectionText(Text::DlgAccessoryPreview, Text::DlgHandbrakePreview, Text::DlgClutchPreview)));
}

void AccessoryDialog::setStatus(const wchar_t* text) const
{
    SetDlgItemTextW(window_, IDC_ACCESSORY_STATUS, text);
}

void AccessoryDialog::deviceTopologyChanged()
{
    inventory_.refreshAfterDeviceChange();
}

void AccessoryDialog::refreshIfIdle()
{
    if (!IsWindowEnabled(window_) || stage_ != CaptureStage::Idle || !captured_.empty())
        return;
    const auto result = inventory_.current();
    if (!result || inventoryRevision_ == result->revision)
        return;
    inventoryRevision_ = result->revision;
    const auto selection = selectedBinding();
    rememberSelectedDevice();
    devices_ = result->devices;
    send(IDC_ACCESSORY_DEVICE, CB_RESETCONTENT);
    for (const auto& item : devices_) {
        auto label = wide(item.name.empty() ? item.id : item.name);
        if (std::ranges::count(devices_, item.name, &DeviceInfo::name) > 1 && item.id.size() == 38)
            label += L" [" + wide(item.id.substr(29, 8)) + L"]";
        send(IDC_ACCESSORY_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    restoreDeviceSelection();
    clearCapture();
    refreshBindings(selection);
}

void AccessoryDialog::refreshDevices()
{
    inventory_.refresh();
    clearCapture();
    setStatus(tr(Text::mainScanningDevices));
}

void AccessoryDialog::clearCapture()
{
    input_.invalidate();
    KillTimer(window_, captureTimer);
    stage_ = CaptureStage::Idle;
    captured_.clear();
    previous_.reset();
    povCapture_.reset();
    captureReadInterrupted_ = false;
    captureHadSignal_ = false;
    rest_.reset();
    send(IDC_ACCESSORY_INPUT, CB_RESETCONTENT);
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, tr(sectionText(Text::DlgAccessoriesCapture, Text::DlgHandbrakeCapture, Text::DlgClutchCapture)));
    if (const auto* device = selectedDevice()) {
        if (analogPage()) {
            setStatus(tr(clutchPage() ? Text::DlgClutchReadyToCapture : Text::DlgHandbrakeReadyToCapture));
        } else {
            const auto status = tr(Text::DlgAccessoriesAxesCount) + std::to_wstring(std::ranges::count(device->axes, true))
                + L" · " + tr(Text::DlgAccessoriesButtonsCount) + std::to_wstring(device->buttons.count())
                + L" · " + tr(Text::DlgAccessoriesDpadsCount) + std::to_wstring(std::ranges::count(device->povs, true))
                + L"\r\n" + tr(Text::DlgAccessoriesAxisHint);
            setStatus(status.c_str());
        }
    } else {
        setStatus(tr(!deviceId_.empty() ? Text::DlgSelectedDeviceDisconnected
            : devices_.empty() ? Text::DlgAccessoriesConnectDevice
            : sectionText(Text::DlgAccessoriesConnectDevice, Text::DlgHandbrakeSelectDevice, Text::DlgClutchSelectDevice)));
    }
    refreshActions();
}

void AccessoryDialog::refreshAnalogActions(bool idle, bool captured)
{
    const auto index = analogBinding();
    std::wstring selectedAxis = tr(clutchPage() ? Text::DlgClutchUnconfigured : Text::DlgHandbrakeUnconfigured);
    if (index) {
        const auto& binding = bindings_[*index];
        const auto device = std::ranges::find(devices_, binding.deviceId, &DeviceInfo::id);
        selectedAxis = accessoryInputLabel(binding) + L" · ";
        selectedAxis += device == devices_.end() ? tr(Text::DlgAccessoryPreviewDevice)
            : wide(device->name.empty() ? binding.deviceId : device->name);
        if (hasDedicatedAccessoryAxisConflict(bindings_)) {
            selectedAxis += L"\r\n";
            selectedAxis += tr(Text::DlgSavedDedicatedAccessoryAxisConflict);
        } else if (savedAxes_ && analogAccessoryAxisConflict(*savedAxes_, binding)) {
            selectedAxis += L"\r\n";
            selectedAxis += tr(Text::DlgSavedAccessoryDriveAxisConflict);
        }
    }
    SetDlgItemTextW(window_, IDC_HANDBRAKE_AXIS, selectedAxis.c_str());
    EnableWindow(GetDlgItem(window_, IDC_HANDBRAKE_REMOVE), idle && index.has_value());
    if (clutchPage()) {
        const auto mode = std::ranges::find(clutchOptions, clutchMode_, &ClutchOption::mode);
        EnableWindow(GetDlgItem(window_, IDC_CLUTCH_MODE), idle);
        SetDlgItemTextW(window_, IDC_CLUTCH_STATUS, tr(mode->hint));
        EnableWindow(GetDlgItem(window_, IDC_HANDBRAKE_APPLY_AXIS), idle && captured);
        return;
    }
    SetDlgItemTextW(window_, IDC_HANDBRAKE_STATUS, validCeiling_ ? L"" : tr(Text::DlgHandbrakeInvalid));
    EnableWindow(GetDlgItem(window_, IDC_HANDBRAKE_CEILING), idle);
    EnableWindow(GetDlgItem(window_, IDC_HANDBRAKE_APPLY_AXIS), idle && captured && validCeiling_);
    EnableWindow(GetDlgItem(window_, IDC_HANDBRAKE_USE_SPACE),
        idle && !index && legacyHandbrakeBinding().has_value() && validCeiling_);
}

void AccessoryDialog::refreshActions()
{
    const auto row = selectedBinding();
    const auto capture = selectedCapture();
    const bool idle = stage_ == CaptureStage::Idle;
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_CAPTURE), selectedDevice() != nullptr);
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_INPUT), idle && !captured_.empty());
    EnableWindow(GetDlgItem(window_, IDOK), modified_ && (validCeiling_ || !handbrakeBinding()) && idle);
    if (analogPage()) {
        refreshAnalogActions(idle, capture.has_value());
    } else {
        const auto* binding = row ? &bindings_[*row] : capture ? &captured_[*capture] : nullptr;
        const bool canEdit = idle && binding;
        EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_ACTIVATION), canEdit);
        send(IDC_ACCESSORY_ACTIVATION, CB_SETCURSEL, !canEdit ? static_cast<WPARAM>(-1)
            : binding->activation == AccessoryActivation::pulse ? 1 : 0);
        EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_KEY), idle);
        EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_ADD), idle && capture.has_value() && isAccessoryKey(key_));
        EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_EDIT), idle && row.has_value());
        EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_THRESHOLDS),
            idle && row.has_value() && bindings_[*row].kind == AccessoryKind::Axis);
        EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_REMOVE), idle && row.has_value());
    }
    selectPreview();
}

void AccessoryDialog::selectCapturedInput()
{
    if (!analogPage())
        send(IDC_ACCESSORY_LIST, LB_SETCURSEL, static_cast<WPARAM>(-1));
    refreshActions();
}

void AccessoryDialog::selectPreview()
{
    std::optional<AccessoryBinding> binding;
    if (stage_ == CaptureStage::Idle) {
        if (analogPage()) {
            if (const auto capture = selectedCapture()) {
                binding = captured_[*capture];
                if (clutchPage())
                    binding->clutchMode = clutchMode_;
                else
                    binding = asHandbrake(*binding, handbrakeCeiling_);
            } else if (const auto row = analogBinding()) {
                binding = bindings_[*row];
            }
        } else if (const auto row = selectedBinding()) {
            binding = bindings_[*row];
        } else if (const auto capture = selectedCapture()) {
            binding = captured_[*capture];
            binding->key = key_;
        }
    }
    if (binding != previewBinding_) {
        input_.invalidate();
        previewBinding_ = std::move(binding);
        preview_.reset();
        if (previewBinding_) {
            auto mapping = *previewBinding_;
            if (!mapping.handbrakeCeilingPercent && !mapping.clutchMode && !isAccessoryKey(mapping.key))
                mapping.key = VK_SPACE; // Use a valid internal key to evaluate an unassigned draft.
            preview_.emplace(AccessoryProfile{std::move(mapping)});
        }
        const bool axis = previewBinding_ && previewBinding_->kind == AccessoryKind::Axis;
        if (axis != previewAxis_) {
            for (const int control : {IDC_ACCESSORY_PREVIEW_METER, IDC_ACCESSORY_PREVIEW_LEVEL,
                                       IDC_ACCESSORY_PREVIEW_LIMITS})
                ShowWindow(GetDlgItem(window_, control), axis ? SW_SHOW : SW_HIDE);
            previewAxis_ = axis;
        }
        if (axis) {
            if (previewBinding_->handbrakeCeilingPercent) {
                setHandbrakePreview({});
            } else if (previewBinding_->clutchMode) {
                SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_LIMITS, L"");
            } else {
                const auto thresholds = previewBinding_->thresholds;
                const auto limits = tr(Text::DlgAccessoryPreviewPress) + percent(static_cast<int>(thresholds.press))
                    + L"  ·  " + tr(Text::DlgAccessoryPreviewRelease) + percent(static_cast<int>(thresholds.release));
                SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_LIMITS, limits.c_str());
            }
            SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_LEVEL,
                                (tr(Text::DlgAccessoryPreviewTravel) + std::wstring(L"—")).c_str());
            InvalidateRect(GetDlgItem(window_, IDC_ACCESSORY_PREVIEW_METER), nullptr, FALSE);
        }
        renderPreview(tr(Text::DlgAccessoryPreviewPaused), std::nullopt);
    }
    setPreviewFocus(GetForegroundWindow() == window_);
}

void AccessoryDialog::activationChanged(bool active)
{
    if (active)
        refreshIfIdle();
    else
        interruptCapture();
    setPreviewFocus(active);
}

void AccessoryDialog::setPreviewFocus(bool focused)
{
    const bool shouldPoll = focused && IsWindowEnabled(window_) && stage_ == CaptureStage::Idle && preview_;
    if (shouldPoll) {
        if (!previewPolling_)
            previewPolling_ = SetTimer(window_, previewTimer, previewIntervalMs, nullptr) != 0;
        return;
    }
    if (previewPolling_) {
        input_.invalidate();
        KillTimer(window_, previewTimer);
        previewPolling_ = false;
        if (preview_)
            preview_->reset();
    }
    renderPreview(tr(stage_ != CaptureStage::Idle || previewBinding_ ? Text::DlgAccessoryPreviewPaused
                                                                     : Text::DlgAccessoryPreviewSelect), std::nullopt);
}

void AccessoryDialog::pollPreview()
{
    if (!previewPolling_)
        return;
    const auto focused = [&] { return GetForegroundWindow() == window_ && IsWindowEnabled(window_); };
    if (!focused()) {
        setPreviewFocus(false);
        return;
    }
    std::array<AccessoryObservation, 1> observations;
    const bool progressive = previewBinding_->handbrakeCeilingPercent != 0;
    std::optional<float> handbrake;
    preview_->poll(input_, observations, nullptr, progressive ? &handbrake : nullptr);
    if (!focused()) {
        setPreviewFocus(false);
        return;
    }
    const auto& observation = observations.front();
    const auto travel = observation.travel
        ? std::optional{static_cast<int>(std::lround(std::clamp(*observation.travel, 0.0, 1.0) * 100.0))}
        : std::nullopt;
    const auto status = [&] {
        switch (observation.status) {
        case AccessoryStatus::deviceUnavailable: return Text::DlgAccessoryPreviewDevice;
        case AccessoryStatus::inputUnavailable: return Text::DlgAccessoryPreviewInput;
        case AccessoryStatus::awaitingFirstReport:
        case AccessoryStatus::temporarilyUnavailable: return Text::DlgAccessoryPreviewPending;
        case AccessoryStatus::waitingForNeutral: return Text::DlgAccessoryPreviewNeutral;
        case AccessoryStatus::armed:
            if (progressive)
                return Text::DlgAccessoryPreviewHandbrake;
            if (previewBinding_->clutchMode)
                return Text::DlgClutchPreviewReady;
            if (!isAccessoryKey(previewBinding_->key))
                return Text::DlgAccessoryPreviewUnassigned;
            if (previewBinding_->activation == AccessoryActivation::pulse)
                return observation.active ? Text::DlgAccessoryPreviewPulseActive : Text::DlgAccessoryPreviewPulseReady;
            return observation.active ? Text::DlgAccessoryPreviewPressed : Text::DlgAccessoryPreviewReleased;
        }
        return Text::DlgAccessoryPreviewInput;
    }();
    auto message = std::wstring(tr(status));
    if (observation.status == AccessoryStatus::armed && isAccessoryKey(previewBinding_->key))
        message = accessoryKeyLabel(previewBinding_->key) + message;
    const auto output = handbrake ? std::optional{static_cast<int>(std::lround(*handbrake * 1000.0f))} : std::nullopt;
    renderPreview(std::move(message), travel, output);
}

void AccessoryDialog::setHandbrakePreview(std::optional<int> tenths) const
{
    const auto cap = percent(static_cast<int>(previewBinding_->handbrakeCeilingPercent));
    std::wstring output = L"—";
    if (tenths) {
        output = std::to_wstring(*tenths / 10);
        if (*tenths % 10)
            output += tr(Text::mainDecimalSeparator) + std::to_wstring(*tenths % 10);
        output += tr(Text::mainPercentSuffix);
    }
    const auto text = tr(Text::DlgAccessoryPreviewHandbrakeCeiling) + cap
        + L"  ·  " + tr(Text::DlgAccessoryPreviewHandbrakeOutput) + output;
    SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_LIMITS, text.c_str());
}

void AccessoryDialog::renderPreview(std::wstring status, std::optional<int> travel, std::optional<int> handbrake)
{
    if (status != previewStatus_) {
        SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_STATUS, status.c_str());
        previewStatus_ = std::move(status);
    }
    if (travel != previewTravel_) {
        previewTravel_ = travel;
        const auto value = tr(Text::DlgAccessoryPreviewTravel) + (travel ? percent(*travel) : L"—");
        SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_LEVEL, value.c_str());
        InvalidateRect(GetDlgItem(window_, IDC_ACCESSORY_PREVIEW_METER), nullptr, FALSE);
    }
    if (handbrake != previewHandbrake_) {
        previewHandbrake_ = handbrake;
        if (previewBinding_ && previewBinding_->handbrakeCeilingPercent)
            setHandbrakePreview(handbrake);
    }
}

void AccessoryDialog::drawPreview(const DRAWITEMSTRUCT& item) const
{
    if (!previewBinding_ || !previewAxis_)
        return;
    const bool contrast = ui::isHighContrast();
    const auto color = [contrast](COLORREF themed, int native) { return contrast ? GetSysColor(native) : themed; };
    const auto rect = item.rcItem;
    if (IsRectEmpty(&rect))
        return;
    const auto x = [&](int percentValue) { return rect.left + MulDiv(rect.right - rect.left, percentValue, 100); };
    ui::fill(item.hDC, rect, color(ui::field, COLOR_BTNFACE));
    if (previewTravel_)
        ui::fill(item.hDC, {rect.left, rect.top, x(*previewTravel_), rect.bottom}, color(ui::cyan, COLOR_HIGHLIGHT));
    if (previewBinding_->handbrakeCeilingPercent || previewBinding_->clutchMode)
        return;
    const auto line = [&](unsigned int value, COLORREF marker) {
        const auto position = std::clamp(x(static_cast<int>(value)), rect.left, rect.right - 1);
        ui::fill(item.hDC, {position, rect.top, position + 1, rect.bottom}, marker);
    };
    line(previewBinding_->thresholds.release, color(ui::ink, COLOR_WINDOWTEXT));
    line(previewBinding_->thresholds.press, color(ui::yellow, COLOR_WINDOWTEXT));
}

void AccessoryDialog::chooseKey()
{
    if (analogPage() || stage_ != CaptureStage::Idle)
        return;
    if (const auto key = chooseAccessoryKey(window_, key_)) {
        key_ = *key;
        const auto label = accessoryKeyLabel(key_) + L"…";
        SetDlgItemTextW(window_, IDC_ACCESSORY_KEY, label.c_str());
        if (!captured_.empty())
            selectCapturedInput();
        else
            refreshActions();
    }
}

void AccessoryDialog::changeActivation()
{
    if (analogPage() || stage_ != CaptureStage::Idle)
        return;
    const auto selected = send(IDC_ACCESSORY_ACTIVATION, CB_GETCURSEL);
    if (selected == CB_ERR)
        return;
    const auto activation = selected == 1 ? AccessoryActivation::pulse : AccessoryActivation::hold;
    if (const auto row = selectedBinding()) {
        auto& binding = bindings_[*row];
        if (binding.activation != activation) {
            binding.activation = activation;
            modified_ = true;
            refreshBindings(row);
            setStatus(tr(Text::DlgAccessoriesActivationEdited));
        }
    } else if (const auto capture = selectedCapture()) {
        captured_[*capture].activation = activation;
        selectPreview();
    }
}

void AccessoryDialog::startSampling(CaptureStage stage, const wchar_t* text)
{
    if (!SetTimer(window_, captureTimer, captureIntervalMs, nullptr)) {
        clearCapture();
        setStatus(tr(Text::DlgAccessoriesCaptureFailed));
        return;
    }
    if (stage == CaptureStage::AxisTravel) {
        // Discard samples from before the click without clearing a connection loss
        // observed since the resting position was measured.
        travelStartedAt_ = DeviceReadClock::now();
    } else {
        input_.invalidate();
    }
    measurements_ = {};
    stage_ = stage;
    captureReadInterrupted_ = false;
    captureHadSignal_ = false;
    deadline_ = GetTickCount64() + captureTimeoutMs;
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, tr(Text::DlgAccessoriesCancelCapture));
    setStatus(text);
    refreshActions();
}

bool AccessoryDialog::rejectDisconnectedRest(const DeviceInfo& device)
{
    const auto current = inventory_.current();
    if (!current || !current->available ||
        std::ranges::find(current->devices, device.id, &DeviceInfo::id) != current->devices.end())
        return false;

    clearCapture();
    setStatus(tr(Text::DlgAccessoriesCaptureDeviceRest));
    return true;
}

void AccessoryDialog::capture()
{
    if (stage_ == CaptureStage::Idle) {
        refreshIfIdle();
        if (!inventory_.current()) {
            setStatus(tr(Text::mainScanningDevices));
            return;
        }
    }
    const auto* device = selectedDevice();
    if (!device)
        return;
    if (stage_ == CaptureStage::AwaitingTravel) {
        if (rejectDisconnectedRest(*device))
            return;
        startSampling(CaptureStage::AxisTravel,
            tr(sectionText(Text::DlgAccessoriesHoldAtLimit, Text::DlgHandbrakeHoldAtLimit, Text::DlgClutchHoldAtLimit)));
        return;
    }
    const auto wasIdle = stage_ == CaptureStage::Idle;
    clearCapture();
    if (!wasIdle)
        return;
    if (analogPage() || send(IDC_ACCESSORY_TYPE, CB_GETCURSEL) == 1) {
        if (std::ranges::none_of(device->axes, [](bool present) { return present; })) {
            setStatus(tr(device->layoutPending.axes.any() ? Text::mainScanningDevices
                                                           : Text::DlgAccessoriesNoAbsoluteAxes));
            return;
        }
        startSampling(CaptureStage::AxisRest,
            tr(sectionText(Text::DlgAccessoriesHoldAtRest, Text::DlgHandbrakeHoldAtRest, Text::DlgClutchHoldAtRest)));
    } else {
        startSampling(CaptureStage::Digital, tr(Text::DlgAccessoryPreviewPending));
    }
}

void AccessoryDialog::interruptCapture()
{
    if (stage_ == CaptureStage::Idle)
        return;
    clearCapture();
    setStatus(tr(Text::DlgAccessoriesCaptureInterrupted));
}

void AccessoryDialog::sample()
{
    if (stage_ == CaptureStage::Idle)
        return;
    const bool awaitingTravel = stage_ == CaptureStage::AwaitingTravel;
    const auto focused = [&] { return IsWindowEnabled(window_) && GetForegroundWindow() == window_; };
    const auto expired = [&] { return !awaitingTravel && GetTickCount64() >= deadline_; };
    if (!focused()) {
        interruptCapture();
        return;
    }
    if (expired()) {
        clearCapture();
        setStatus(tr(Text::DlgAccessoriesCaptureExpired));
        return;
    }
    const auto* device = selectedDevice();
    if (device && (awaitingTravel || stage_ == CaptureStage::AxisTravel) && rejectDisconnectedRest(*device))
        return;
    const auto reading = device ? input_.capture(device->id) : CaptureDeviceRead{};
    if (!reading.updated)
        return;
    const auto& state = reading.sample;
    if (!focused()) {
        interruptCapture();
        return;
    }
    if (expired()) {
        clearCapture();
        setStatus(tr(Text::DlgAccessoriesCaptureExpired));
        return;
    }
    if (reading.needsRest()) {
        if (stage_ != CaptureStage::Digital &&
            restartAnalogCapture(reading, stage_ == CaptureStage::AxisRest, captureHadSignal_)) {
            clearCapture();
            setStatus(tr(Text::DlgAccessoriesCaptureDeviceRest));
            return;
        }
        previous_.reset();
        povCapture_.reset();
    }
    if (awaitingTravel || (stage_ == CaptureStage::AxisTravel && state && state->sampledAt < travelStartedAt_))
        return;
    if (!state) {
        measurements_ = {};
        povCapture_.reset();
        if (!captureReadInterrupted_)
            setStatus(tr(stage_ == CaptureStage::AxisRest && !captureHadSignal_
                ? Text::DlgAccessoryPreviewPending : Text::DlgShifterDeviceReadInterrupted));
        captureReadInterrupted_ = true;
        return;
    }
    const bool recovered = std::exchange(captureReadInterrupted_, false);
    if (stage_ == CaptureStage::Digital) {
        povCapture_.observe(*state);
        if (recovered || !previous_)
            setStatus(tr(Text::DlgAccessoriesPressInput));
        if (!previous_) {
            previous_ = state;
            return;
        }
        const auto pressed = state->buttons & ~previous_->buttons;
        for (std::size_t i = 0; i < buttonCount; ++i) {
            if (pressed[i])
                captured_.push_back({device->id, AccessoryKind::Button, i, 0, 1, 0});
        }
        for (std::size_t i = 0; i < povCount; ++i) {
            const auto direction = quantizePov(state->povs[i]);
            if (povCapture_.armed(i) && direction != povCentered && direction != quantizePov(previous_->povs[i]))
                captured_.push_back({device->id, AccessoryKind::Pov, i, povCentered, direction, 0});
        }
        previous_ = state;
        if (!captured_.empty())
            finishCapture();
        return;
    }
    if (recovered) {
        setStatus(tr(stage_ == CaptureStage::AxisRest
            ? sectionText(Text::DlgAccessoriesHoldAtRest, Text::DlgHandbrakeHoldAtRest, Text::DlgClutchHoldAtRest)
            : sectionText(Text::DlgAccessoriesHoldAtLimit, Text::DlgHandbrakeHoldAtLimit, Text::DlgClutchHoldAtLimit)));
    }
    if (!measurements_.add(*state)) {
        measurements_ = {};
        return;
    }
    captureHadSignal_ = true;
    const auto measured = measurements_.result();
    if (!measured)
        return;
    if (stage_ == CaptureStage::AxisRest) {
        rest_ = measured;
        stage_ = CaptureStage::AwaitingTravel;
        SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE,
            tr(analogPage() ? Text::DlgHandbrakeMeasureLimit : Text::DlgAccessoriesMeasureLimit));
        setStatus(tr(sectionText(Text::DlgAccessoriesMeasureLimitHint, Text::DlgHandbrakeMeasureLimitHint, Text::DlgClutchMeasureLimitHint)));
        return;
    }
    for (const auto& candidate : axisCandidates(*rest_, *rest_, *measured, false)) {
        const auto i = candidate.axis;
        captured_.push_back({device->id, AccessoryKind::Axis, i, rest_->median[i], measured->median[i], 0});
    }
    finishCapture();
}

void AccessoryDialog::finishCapture()
{
    KillTimer(window_, captureTimer);
    stage_ = CaptureStage::Idle;
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE,
        tr(sectionText(Text::DlgAccessoriesCapture, Text::DlgHandbrakeCapture, Text::DlgClutchCapture)));
    for (const auto& binding : captured_) {
        const auto label = accessoryInputLabel(binding);
        send(IDC_ACCESSORY_INPUT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    send(IDC_ACCESSORY_INPUT, CB_SETCURSEL, 0);
    if (captured_.empty()) {
        setStatus(tr(Text::DlgAccessoriesAxisDidNotMove));
    } else if (analogPage()) {
        setStatus(tr(captured_.size() == 1 ? Text::DlgHandbrakeCapturedAxis
            : clutchPage() ? Text::DlgClutchMultipleAxes : Text::DlgHandbrakeMultipleAxes));
    } else {
        setStatus(tr(captured_.size() == 1 ? Text::DlgAccessoriesChooseKeyAndAdd : Text::DlgAccessoriesMultipleInputs));
    }
    selectCapturedInput();
}

void AccessoryDialog::refreshBindings(std::optional<std::size_t> selection)
{
    const auto top = send(IDC_ACCESSORY_LIST, LB_GETTOPINDEX);
    send(IDC_ACCESSORY_LIST, LB_RESETCONTENT);
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        const auto& binding = bindings_[i];
        if (binding.role != ShifterRole::None || binding.handbrakeCeilingPercent || binding.clutchMode)
            continue;
        auto label = accessoryKeyLabel(binding.key) + L"  ←  " + accessoryInputLabel(binding)
            + L"  ·  " + accessoryActivationLabel(binding.activation);
        if (binding.kind == AccessoryKind::Axis) {
            label += L" · " + std::to_wstring(binding.thresholds.press) + L"% / "
                + std::to_wstring(binding.thresholds.release) + L"%";
        }
        label += L"  ·  " + deviceLabel(binding.deviceId);
        const auto row = send(IDC_ACCESSORY_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (row >= 0) {
            send(IDC_ACCESSORY_LIST, LB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(i));
            if (selection == i)
                send(IDC_ACCESSORY_LIST, LB_SETCURSEL, static_cast<WPARAM>(row));
        }
    }
    if (selection && top >= 0)
        send(IDC_ACCESSORY_LIST, LB_SETTOPINDEX, static_cast<WPARAM>(top));
    refreshActions();
}

void AccessoryDialog::add()
{
    const auto input = selectedCapture();
    if (analogPage() || stage_ != CaptureStage::Idle || !input || !isAccessoryKey(key_))
        return;
    if (std::ranges::find(bindings_, key_, &AccessoryBinding::key) != bindings_.end()) {
        setStatus(tr(Text::DlgAccessoriesKeyAlreadyAssigned));
        return;
    }
    if (bindings_.size() >= maxAccessoryBindings) {
        setStatus(tr(Text::DlgAccessoriesBindingLimit));
        return;
    }
    auto binding = captured_[*input];
    if (std::ranges::any_of(bindings_, [&](const auto& saved) {
            return isNativeTransmissionBinding(saved) && sameShifterInput(binding, saved);
        })) {
        setStatus(tr(Text::DlgAccessoryNativeInputConflict));
        return;
    }
    if (std::ranges::any_of(bindings_, [&](const auto& saved) {
            return saved.clutchMode && sameShifterInput(binding, saved);
        })) {
        setStatus(tr(Text::DlgClutchInputConflict));
        return;
    }
    if (key_ == VK_SPACE && std::ranges::any_of(bindings_, [&](const auto& saved) {
            return saved.handbrakeCeilingPercent && sameShifterInput(binding, saved);
        })) {
        setStatus(tr(Text::DlgHandbrakeInputConflict));
        return;
    }
    binding.key = key_;
    bindings_.push_back(std::move(binding));
    modified_ = true;
    clearCapture();
    refreshBindings(bindings_.size() - 1);
    setStatus(tr(Text::DlgAccessoriesBindingAdded));
}

void AccessoryDialog::edit()
{
    if (stage_ != CaptureStage::Idle)
        return;
    const auto selection = selectedBinding();
    if (!selection)
        return;
    switch (chooseBindingKey(window_, bindings_, *selection)) {
    case BindingKeyEdit::unchanged:
        break;
    case BindingKeyEdit::occupied:
        setStatus(tr(Text::DlgAccessoriesKeyAlreadyAssigned));
        break;
    case BindingKeyEdit::handbrakeInputConflict:
        setStatus(tr(Text::DlgHandbrakeInputConflict));
        break;
    case BindingKeyEdit::changed:
        modified_ = true;
        refreshBindings(selection);
        setStatus(tr(Text::DlgAccessoriesBindingEdited));
        break;
    }
}

void AccessoryDialog::editThresholds()
{
    if (stage_ != CaptureStage::Idle)
        return;
    const auto selection = selectedBinding();
    if (!selection)
        return;
    auto& binding = bindings_[*selection];
    if (binding.kind != AccessoryKind::Axis)
        return;
    if (const auto values = editAccessoryThresholds(window_, binding.thresholds); values && *values != binding.thresholds) {
        binding.thresholds = *values;
        modified_ = true;
        refreshBindings(selection);
        setStatus(tr(Text::DlgAccessoriesThresholdsEdited));
    }
}

void AccessoryDialog::applyHandbrake(AccessoryBinding binding)
{
    if (std::ranges::any_of(bindings_, [&](const auto& saved) {
            return saved.clutchMode && sameShifterInput(binding, saved);
        })) {
        setStatus(tr(Text::DlgClutchInputConflict));
        return;
    }
    if (std::ranges::any_of(bindings_, [&](const auto& saved) {
            return isNativeTransmissionBinding(saved) && sameShifterInput(binding, saved);
        })) {
        setStatus(tr(Text::DlgAccessoryNativeInputConflict));
        return;
    }
    auto target = handbrakeBinding();
    const auto space = std::ranges::find(bindings_, static_cast<unsigned int>(VK_SPACE), &AccessoryBinding::key);
    if (space != bindings_.end() && sameShifterInput(binding, *space)) {
        if (target || !isLegacyHandbrakeAxis(*space)) {
            setStatus(tr(Text::DlgHandbrakeInputConflict));
            return;
        }
        target = static_cast<std::size_t>(space - bindings_.begin());
    }
    if (!target && bindings_.size() == maxAccessoryBindings) {
        setStatus(tr(Text::DlgAccessoriesBindingLimit));
        return;
    }

    binding = asHandbrake(std::move(binding), handbrakeCeiling_);
    if (savedAxes_ && analogAccessoryAxisConflict(*savedAxes_, binding)) {
        setStatus(tr(Text::DlgAccessoryDriveAxisConflict));
        return;
    }
    deviceId_ = binding.deviceId;
    if (!target) {
        bindings_.push_back(std::move(binding));
        modified_ = true;
    } else if (bindings_[*target] != binding) {
        bindings_[*target] = std::move(binding);
        modified_ = true;
    }
    selectDevice(deviceId_);
    clearCapture();
    refreshBindings();
    setStatus(tr(Text::DlgHandbrakeChanged));
}

void AccessoryDialog::applyClutch(AccessoryBinding binding)
{
    if (std::ranges::any_of(bindings_, [&](const auto& saved) {
            return !saved.clutchMode && sameShifterInput(binding, saved);
        })) {
        setStatus(tr(Text::DlgClutchInputConflict));
        return;
    }
    const auto target = clutchBinding();
    if (!target && bindings_.size() == maxAccessoryBindings) {
        setStatus(tr(Text::DlgAccessoriesBindingLimit));
        return;
    }
    binding.clutchMode = clutchMode_;
    if (savedAxes_ && analogAccessoryAxisConflict(*savedAxes_, binding)) {
        setStatus(tr(Text::DlgAccessoryDriveAxisConflict));
        return;
    }
    deviceId_ = binding.deviceId;
    if (!target) {
        bindings_.push_back(std::move(binding));
        modified_ = true;
    } else if (bindings_[*target] != binding) {
        bindings_[*target] = std::move(binding);
        modified_ = true;
    }
    selectDevice(deviceId_);
    clearCapture();
    refreshBindings();
    setStatus(tr(Text::DlgClutchChanged));
}

void AccessoryDialog::useCapturedAnalog()
{
    if (!analogPage() || stage_ != CaptureStage::Idle || (handbrakePage() && !validCeiling_))
        return;
    if (const auto capture = selectedCapture()) {
        if (clutchPage())
            applyClutch(captured_[*capture]);
        else
            applyHandbrake(captured_[*capture]);
    }
}

void AccessoryDialog::changeClutchMode()
{
    const auto row = send(IDC_CLUTCH_MODE, CB_GETCURSEL);
    if (!clutchPage() || stage_ != CaptureStage::Idle || row < 0 ||
        static_cast<std::size_t>(row) >= clutchOptions.size())
        return;
    clutchMode_ = clutchOptions[static_cast<std::size_t>(row)].mode;
    if (const auto index = clutchBinding(); index && bindings_[*index].clutchMode != clutchMode_) {
        bindings_[*index].clutchMode = clutchMode_;
        modified_ = true;
        setStatus(tr(Text::DlgClutchChanged));
    }
    refreshActions();
}

void AccessoryDialog::useLegacyHandbrake()
{
    if (!handbrakePage() || stage_ != CaptureStage::Idle || !validCeiling_ || handbrakeBinding())
        return;
    if (const auto binding = legacyHandbrakeBinding())
        applyHandbrake(bindings_[*binding]);
}

void AccessoryDialog::changeHandbrakeCeiling()
{
    if (!handbrakePage())
        return;
    const auto ceiling = readPercent(window_, IDC_HANDBRAKE_CEILING);
    validCeiling_ = validHandbrakeCeilingPercent(ceiling);
    if (validCeiling_) {
        handbrakeCeiling_ = ceiling;
        if (const auto binding = handbrakeBinding(); binding && bindings_[*binding].handbrakeCeilingPercent != ceiling) {
            bindings_[*binding].handbrakeCeilingPercent = ceiling;
            modified_ = true;
        }
    }
    refreshActions();
}

void AccessoryDialog::removeAnalog()
{
    if (!analogPage() || stage_ != CaptureStage::Idle)
        return;
    const auto index = analogBinding();
    if (!index)
        return;
    bindings_.erase(bindings_.begin() + *index);
    modified_ = true;
    if (handbrakePage()) {
        handbrakeCeiling_ = defaultHandbrakeCeilingPercent;
        validCeiling_ = true;
        SetDlgItemInt(window_, IDC_HANDBRAKE_CEILING, handbrakeCeiling_, FALSE);
    } else {
        clutchMode_ = ClutchMode::off;
        send(IDC_CLUTCH_MODE, CB_SETCURSEL, 0);
    }
    clearCapture();
    refreshBindings();
    setStatus(tr(Text::DlgAccessoriesBindingRemoved));
}

void AccessoryDialog::remove()
{
    const auto index = selectedBinding();
    if (!index || stage_ != CaptureStage::Idle)
        return;
    bindings_.erase(bindings_.begin() + *index);
    modified_ = true;
    refreshBindings();
    setStatus(tr(Text::DlgAccessoriesBindingRemoved));
}

void AccessoryDialog::save()
{
    const bool ready = modified_ && stage_ == CaptureStage::Idle && (validCeiling_ || !handbrakeBinding());
    if (ready && saveAccessorySettings(window_, root_, preset_, revision_, bindings_))
        EndDialog(window_, IDOK);
}

void AccessoryDialog::cancel()
{
    if (!modified_ || MessageBoxW(window_, tr(Text::DlgAccessoriesDiscard),
        tr(Text::DlgAccessoriesTitle), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        EndDialog(window_, IDCANCEL);
}

INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        auto& dialog = *reinterpret_cast<AccessoryDialog*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        const auto heading = tr(dialog.heading());
        const std::wstring title = std::wstring(heading) + L" · Chrome Axis";
        SetWindowTextW(window, title.c_str());
        SetDlgItemTextW(window, IDC_CLUTCH_MODE_LABEL, tr(Text::DlgClutchMode));
        SetDlgItemTextW(window, IDC_MODAL_DEVICE_LABEL, tr(Text::DlgDevice));
        SetDlgItemTextW(window, IDC_ACCESSORY_REFRESH, tr(Text::DlgRefresh));
        SetDlgItemTextW(window, IDC_ACCESSORY_TYPE_LABEL, tr(Text::DlgAccessoriesInputType));
        SetDlgItemTextW(window, IDC_ACCESSORY_INPUT_LABEL, tr(Text::DlgAccessoriesCapturedInput));
        SetDlgItemTextW(window, IDC_MODAL_KEY_LABEL, tr(Text::DlgKey));
        SetDlgItemTextW(window, IDC_ACCESSORY_KEY, tr(Text::DlgChooseKey));
        SetDlgItemTextW(window, IDC_ACCESSORY_ADD, tr(Text::DlgAccessoriesAdd));
        SetDlgItemTextW(window, IDC_MODAL_LIST_HEADING, tr(Text::DlgAccessoriesSavedList));
        SetDlgItemTextW(window, IDC_ACCESSORY_ACTIVATION_LABEL, tr(Text::DlgAccessoryActivation));
        SetDlgItemTextW(window, IDC_ACCESSORY_EDIT, tr(Text::DlgBindingEdit));
        SetDlgItemTextW(window, IDC_ACCESSORY_THRESHOLDS, tr(Text::DlgAccessoriesThresholds));
        SetDlgItemTextW(window, IDC_HANDBRAKE_USE_SPACE, tr(Text::DlgHandbrakeUseSpace));
        SetDlgItemTextW(window, IDC_HANDBRAKE_APPLY_AXIS, tr(Text::DlgHandbrakeApplyAxis));
        SetDlgItemTextW(window, IDC_HANDBRAKE_REMOVE, tr(Text::DlgHandbrakeRemove));
        SetDlgItemTextW(window, IDC_HANDBRAKE_CEILING_LABEL, tr(Text::DlgHandbrakeCeiling));
        SetDlgItemTextW(window, IDC_ACCESSORY_PREVIEW_HEADING, tr(Text::DlgAccessoryPreview));
        SetDlgItemTextW(window, IDC_ACCESSORY_REMOVE, tr(Text::DlgAccessoriesRemove));
        SetDlgItemTextW(window, IDOK, tr(Text::DlgSave));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, heading, IDC_ACCESSORY_STATUS, IDC_ACCESSORY_LIST,
                            {IDC_ACCESSORY_CAPTURE, IDC_ACCESSORY_ADD, IDC_HANDBRAKE_APPLY_AXIS, IDOK},
                            DialogDecoration::scrollable);
        return dialog.initialise(window);
    }
    auto* dialog = reinterpret_cast<AccessoryDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog)
        return FALSE;
    if (message == WM_TIMER) {
        if (wParam == captureTimer) {
            dialog->sample();
            return TRUE;
        }
        if (wParam == previewTimer) {
            dialog->pollPreview();
            return TRUE;
        }
        if (wParam == inventoryTimer) {
            dialog->refreshIfIdle();
            return TRUE;
        }
    }
    if (message == WM_DEVICECHANGE &&
        (wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)) {
        dialog->deviceTopologyChanged();
        return FALSE;
    }
    if (message == WM_ACTIVATE || message == WM_ENABLE) {
        const bool focused = message == WM_ACTIVATE ? LOWORD(wParam) != WA_INACTIVE : wParam != 0;
        dialog->activationChanged(focused);
        return FALSE;
    }
    if (message == WM_DRAWITEM && wParam == IDC_ACCESSORY_PREVIEW_METER) {
        dialog->drawPreview(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    }
    if (message == WM_CLOSE) {
        dialog->cancel();
        return TRUE;
    }
    if (message != WM_COMMAND)
        return FALSE;
    switch (LOWORD(wParam)) {
    case IDC_ACCESSORY_DEVICE:
    case IDC_ACCESSORY_TYPE:
        if (HIWORD(wParam) == CBN_SELCHANGE)
            dialog->clearCapture();
        return TRUE;
    case IDC_ACCESSORY_INPUT:
        if (HIWORD(wParam) == CBN_SELCHANGE)
            dialog->selectCapturedInput();
        return TRUE;
    case IDC_ACCESSORY_ACTIVATION:
        if (HIWORD(wParam) == CBN_SELCHANGE)
            dialog->changeActivation();
        return TRUE;
    case IDC_ACCESSORY_LIST:
        if (HIWORD(wParam) == LBN_DBLCLK)
            dialog->edit();
        else
            dialog->refreshActions();
        return TRUE;
    case IDC_ACCESSORY_KEY:
        if (HIWORD(wParam) == BN_CLICKED)
            dialog->chooseKey();
        return TRUE;
    case IDC_ACCESSORY_REFRESH:
        dialog->refreshDevices();
        return TRUE;
    case IDC_ACCESSORY_CAPTURE:
        dialog->capture();
        return TRUE;
    case IDC_ACCESSORY_ADD:
        dialog->add();
        return TRUE;
    case IDC_ACCESSORY_EDIT:
        dialog->edit();
        return TRUE;
    case IDC_ACCESSORY_THRESHOLDS:
        dialog->editThresholds();
        return TRUE;
    case IDC_CLUTCH_MODE:
        if (HIWORD(wParam) == CBN_SELCHANGE)
            dialog->changeClutchMode();
        return TRUE;
    case IDC_HANDBRAKE_APPLY_AXIS:
        dialog->useCapturedAnalog();
        return TRUE;
    case IDC_HANDBRAKE_USE_SPACE:
        dialog->useLegacyHandbrake();
        return TRUE;
    case IDC_HANDBRAKE_CEILING:
        if (HIWORD(wParam) == EN_CHANGE)
            dialog->changeHandbrakeCeiling();
        return TRUE;
    case IDC_HANDBRAKE_REMOVE:
        dialog->removeAnalog();
        return TRUE;
    case IDC_ACCESSORY_REMOVE:
        dialog->remove();
        return TRUE;
    case IDOK:
        dialog->save();
        return TRUE;
    case IDCANCEL:
        dialog->cancel();
        return TRUE;
    }
    return FALSE;
}

} // namespace

std::optional<AccessoryThresholds> editAccessoryThresholds(HWND owner, const AccessoryThresholds& settings)
{
    auto values = settings;
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ACCESSORY_THRESHOLDS),
        owner, thresholdsProc, reinterpret_cast<LPARAM>(&values));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgThresholdsOpenError), tr(Text::DlgThresholdsTitle), MB_OK | MB_ICONERROR);
    return result == IDOK ? std::optional{values} : std::nullopt;
}

bool editAccessories(HWND owner, AppInput& input, DeviceInventory& inventory, const std::filesystem::path& root,
                     const ProfilePreset& preset, AccessorySection section)
{
    AccessoryDialog dialog(input, inventory, root, preset, section);
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ACCESSORIES), owner,
        dialogProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgAccessoriesOpenError), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONERROR);
    return result == IDOK;
}

bool editProfileShifter(HWND owner, AppInput& input, DeviceInventory& inventory, const std::filesystem::path& root,
                        const ProfilePreset& preset, TransmissionShortcut& shortcut)
{
    const auto path = preset.directory / accessoryFilename;
    const auto revision = profileFileRevision(path);
    auto bindings = readAccessorySettings(owner, path);
    return bindings && editShifter(owner, input, inventory, *bindings, root, shortcut,
        [&](HWND dialog, const AccessoryProfile& updated) { return saveAccessorySettings(dialog, root, preset, revision, updated); });
}

} // namespace chrome_axis
