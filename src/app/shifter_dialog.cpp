#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "device_capture.hpp"
#include "device_inventory.hpp"
#include "transmission_shortcut.hpp"
#include "dialog_theme.hpp"

#include "chrome_axis/calibration.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/keyboard_dialog.hpp"
#include "chrome_axis/presets.hpp"
#include "chrome_axis/shifter.hpp"

#include <dbt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace chrome_axis {
namespace {

constexpr UINT_PTR captureTimer = 1;
constexpr UINT captureInterval = 20;
constexpr ULONGLONG captureTimeout = 30000;
constexpr UINT_PTR inventoryTimer = 2;
constexpr UINT inventoryWatchMs = 80;
constexpr UINT_PTR previewTimer = 3;
constexpr UINT previewInterval = 50;

bool isNumpadDigit(unsigned int key)
{
    return key >= VK_NUMPAD0 && key <= VK_NUMPAD9;
}

std::wstring bindingSummary(const AccessoryBinding& binding, bool nativeVerified)
{
    auto summary = accessoryBindingInputLabel(binding) + L"  →  " + accessoryBindingOutputLabel(binding, nativeVerified);
    if (!isNativeTransmissionBinding(binding))
        summary += L"  ·  " + std::wstring(accessoryActivationLabel(binding.activation));
    return summary;
}

struct ModeOption {
    Text label;
    std::size_t forward;
    bool native;
};

constexpr std::array modes{
    ModeOption{Text::DlgShifterNativeSequential, 0, true},
    ModeOption{Text::DlgShifterSequential, 0, false},
    ModeOption{Text::DlgShifterNativeH6, 6, true},
    ModeOption{Text::DlgShifterNativeH7, 7, true},
    ModeOption{Text::DlgShifterNativeH8, 8, true},
    ModeOption{Text::DlgShifterH6, 6, false},
    ModeOption{Text::DlgShifterH7, 7, false},
    ModeOption{Text::DlgShifterH8, 8, false},
};

enum class Stage { Setup, Neutral, Position, Paused, AnalogReady, AnalogMeasure, Captured, Done };

class ShifterDialog {
public:
    ShifterDialog(DirectInput& input, DeviceInventory& inventory, AccessoryProfile& bindings,
                  const std::filesystem::path& profileRoot, TransmissionShortcut& shortcut,
                  std::function<bool(HWND, const AccessoryProfile&)> save)
        : input_(input), inventory_(inventory), bindings_(bindings), profileRoot_(profileRoot),
          shortcut_(shortcut), save_(std::move(save)), edited_(bindings) {}

    void initialise(HWND window);
    void start();
    void poll();
    void advance();
    void skip();
    void updateControls();
    void selectDevice();
    void selectMode();
    void deviceTopologyChanged();
    bool refreshDevices();
    void chooseKey();
    void chooseShortcut();
    void editSavedKey();
    void editSavedThresholds();
    void removeSaved();
    void previewFocusChanged(bool active);
    void pollPreview();
    void changeActivation();
    void apply();
    void cancel();

private:
    LRESULT send(int control, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) const
    {
        return SendDlgItemMessageW(window_, control, message, wParam, lParam);
    }
    void status(const wchar_t* text);
    void updateNumLockHint();
    void position();
    void captured(AccessoryBinding binding);
    void runTimer();
    void interruptCapture(Text analogMessage, Text positionMessage, Text neutralMessage);
    bool hasCaptureChanges() const { return !draft_.empty() || pending_.has_value() || absentNativePositions_ != 0; }
    bool confirm(Text message) const;
    ShifterRole role() const;
    unsigned int suggestedKey() const;
    void setKey(unsigned int key);
    void enterSetup();
    void showSavedBindings(LRESULT selection = LB_ERR);
    void fillDevices(std::string_view preferredDevice);
    Text setupStatus() const;
    const DeviceInfo* selectedDevice(bool attachedOnly = false) const;
    std::optional<std::size_t> selectedBinding() const;
    void selectPreview(const AccessoryBinding* binding);
    void showPreview(std::wstring text);
    const ModeOption& selectedMode() const;
    bool replacesSetup(const AccessoryBinding& binding) const;
    std::size_t initialMode() const;
    void showShortcut() const;
    bool legacyNativeMode() const;

    DirectInput& input_;
    DeviceInventory& inventory_;
    AccessoryProfile& bindings_;
    const std::filesystem::path& profileRoot_;
    TransmissionShortcut& shortcut_;
    std::function<bool(HWND, const AccessoryProfile&)> save_;
    HWND window_{};
    std::vector<DeviceInfo> devices_;
    std::size_t attachedCount_{};
    AccessoryProfile edited_;
    std::vector<std::size_t> savedRows_;
    std::optional<AccessoryBinding> previewBinding_;
    std::optional<AccessoryReader> preview_;
    std::size_t previewIndex_{};
    std::wstring previewStatus_;
    bool previewRunning_{};
    AccessoryProfile draft_;
    std::optional<AccessoryBinding> pending_;
    std::optional<ShifterCapture> digital_;
    std::optional<AxisCapture> rest_;
    CalibrationCapture measurements_;
    std::string deviceId_;
    std::wstring status_;
    std::size_t step_{};
    std::size_t forward_{};
    unsigned int key_{};
    unsigned int absentNativePositions_{};
    Stage stage_{Stage::Setup};
    ULONGLONG deadline_{};
    bool analog_{};
    bool native_{};
    std::optional<std::uint64_t> inventoryRevision_;
};

void ShifterDialog::initialise(HWND window)
{
    window_ = window;
    shortcut_.refresh(profileRoot_);
    showShortcut();
    SetTimer(window_, inventoryTimer, inventoryWatchMs, nullptr);
    if (const auto result = inventory_.current()) {
        inventoryRevision_ = result->revision;
        devices_ = result->devices;
    }
    SetDlgItemTextW(window_, IDOK, tr(Text::DlgSave));
    auto saved = std::ranges::find_if(edited_, isNativeTransmissionBinding);
    if (saved == edited_.end()) {
        saved = std::ranges::find_if(edited_, [](const AccessoryBinding& binding) {
            return binding.role != ShifterRole::None;
        });
    }
    fillDevices(saved == edited_.end() ? std::string_view{} : saved->deviceId);
    const auto initial = initialMode();
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const auto& mode = modes[i];
        if (mode.native && mode.forward == 8 && initial != i)
            continue;
        const auto row = send(IDC_SHIFTER_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(mode.label)));
        if (row < 0)
            continue;
        if (send(IDC_SHIFTER_MODE, CB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(i)) == CB_ERR)
            send(IDC_SHIFTER_MODE, CB_DELETESTRING, static_cast<WPARAM>(row));
        else if (i == initial)
            send(IDC_SHIFTER_MODE, CB_SETCURSEL, static_cast<WPARAM>(row));
    }
    for (const auto* signal : {tr(Text::DlgDigital), tr(Text::DlgAnalog)})
        send(IDC_SHIFTER_SIGNAL, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(signal));
    populateAccessoryActivation(window_, IDC_SHIFTER_ACTIVATION);
    send(IDC_SHIFTER_SIGNAL, CB_SETCURSEL, 0);
    selectDevice();
}

void ShifterDialog::fillDevices(std::string_view preferredDevice)
{
    send(IDC_SHIFTER_DEVICE, CB_RESETCONTENT);
    attachedCount_ = devices_.size();
    for (const auto& binding : edited_) {
        if (binding.role != ShifterRole::None &&
            std::ranges::find(devices_, binding.deviceId, &DeviceInfo::id) == devices_.end()) {
            DeviceInfo offline;
            offline.id = binding.deviceId;
            devices_.push_back(std::move(offline));
        }
    }
    WPARAM selection = 0;
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        const auto& device = devices_[i];
        const auto& name = device.name.empty() ? device.id : device.name;
        const auto length = static_cast<int>(name.size());
        const int count = MultiByteToWideChar(CP_UTF8, 0, name.data(), length, nullptr, 0);
        std::wstring display(static_cast<std::size_t>(count), L'\0');
        if (count > 0)
            MultiByteToWideChar(CP_UTF8, 0, name.data(), length, display.data(), count);
        if (i >= attachedCount_)
            display += tr(Text::DlgShifterOfflineDevice);
        else if (!device.name.empty() && device.id.size() == 38 && std::ranges::count(devices_, device.name, &DeviceInfo::name) > 1)
            display.append(L" [").append(device.id.begin() + 1, device.id.begin() + 9).append(L"]");
        const auto row = send(IDC_SHIFTER_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        if (row < 0)
            continue;
        if (send(IDC_SHIFTER_DEVICE, CB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(i)) == CB_ERR) {
            send(IDC_SHIFTER_DEVICE, CB_DELETESTRING, static_cast<WPARAM>(row));
            continue;
        }
        if (device.id == preferredDevice)
            selection = static_cast<WPARAM>(row);
    }
    send(IDC_SHIFTER_DEVICE, CB_SETCURSEL, selection);
}

void ShifterDialog::deviceTopologyChanged()
{
    inventory_.refreshAfterDeviceChange();
}

bool ShifterDialog::refreshDevices()
{
    if (stage_ != Stage::Setup || !IsWindowEnabled(window_))
        return false;
    const auto result = inventory_.current();
    if (!result || inventoryRevision_ == result->revision)
        return false;
    const auto* selected = selectedDevice();
    const auto preferred = selected ? selected->id : std::string{};
    inventoryRevision_ = result->revision;
    devices_ = result->devices;
    fillDevices(preferred);
    selectDevice();
    return true;
}

void ShifterDialog::status(const wchar_t* text)
{
    if (status_ != text) {
        status_ = text;
        SetDlgItemTextW(window_, IDC_SHIFTER_STATUS, text);
    }
}

void ShifterDialog::updateControls()
{
    const bool setup = stage_ == Stage::Setup;
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_SHORTCUT), setup || stage_ == Stage::Done);
    const bool sequential = selectedMode().forward == 0;
    const bool canAdvance = stage_ == Stage::Captured || stage_ == Stage::AnalogReady || stage_ == Stage::Paused;
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_DEVICE), setup && !devices_.empty());
    const bool canCapture = selectedDevice(true) != nullptr;
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_MODE), setup && canCapture);
    if (setup && !sequential)
        send(IDC_SHIFTER_SIGNAL, CB_SETCURSEL, 0);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_SIGNAL), setup && canCapture && sequential);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_START), !setup || (canCapture && !legacyNativeMode()));
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_ADVANCE), canAdvance);
    const bool nativeMode = setup ? selectedMode().native : native_;
    for (const int control : {IDC_MODAL_KEY_LABEL, IDC_SHIFTER_KEY})
        ShowWindow(GetDlgItem(window_, control), nativeMode ? SW_HIDE : SW_SHOW);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_KEY), !nativeMode && canAdvance);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_SKIP), forward_ != 0 &&
        (stage_ == Stage::Position || stage_ == Stage::Paused || (!native_ && stage_ == Stage::Captured)));
    SetDlgItemTextW(window_, IDC_SHIFTER_SKIP, tr(nativeMode ? Text::DlgShifterNativeMissing : Text::DlgShifterSkip));
    ShowWindow(GetDlgItem(window_, IDC_SHIFTER_SKIP), setup ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(window_, IDC_SHIFTER_EDIT), setup ? SW_SHOW : SW_HIDE);
    const auto selected = selectedBinding();
    const auto* saved = selected ? &edited_[*selected] : nullptr;
    const bool editable = saved && !isNativeTransmissionBinding(*saved);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_EDIT), editable);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_REMOVE), saved != nullptr);
    ShowWindow(GetDlgItem(window_, IDC_SHIFTER_REMOVE), setup ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(window_, IDC_SHIFTER_THRESHOLDS),
               editable && saved->kind == AccessoryKind::Axis ? SW_SHOW : SW_HIDE);
    selectPreview(saved);
    const auto* activationBinding = saved ? saved : stage_ == Stage::Captured && pending_ ? &*pending_ : nullptr;
    const bool editableActivation = activationBinding && !isNativeTransmissionBinding(*activationBinding);
    const bool showActivation = activationBinding ? editableActivation : !nativeMode;
    for (const int control : {IDC_SHIFTER_ACTIVATION_LABEL, IDC_SHIFTER_ACTIVATION})
        ShowWindow(GetDlgItem(window_, control), showActivation ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_ACTIVATION), editableActivation);
    send(IDC_SHIFTER_ACTIVATION, CB_SETCURSEL, !editableActivation ? static_cast<WPARAM>(-1)
        : activationBinding->activation == AccessoryActivation::pulse ? 1 : 0);
    EnableWindow(GetDlgItem(window_, IDOK), (setup && edited_ != bindings_) ||
        (stage_ == Stage::Done && (!draft_.empty() || edited_ != bindings_)));
    SetDlgItemTextW(window_, IDC_SHIFTER_START, setup ? tr(Text::DlgShifterStart) : tr(Text::DlgShifterRestart));
    SetDlgItemTextW(window_, IDC_SHIFTER_ADVANCE, stage_ == Stage::AnalogReady ? tr(Text::DlgShifterMeasure)
        : stage_ == Stage::Paused ? tr(Text::DlgShifterResume) : tr(Text::DlgShifterNext));
    updateNumLockHint();
}

void ShifterDialog::updateNumLockHint()
{
    const bool setup = stage_ == Stage::Setup;
    const auto& mode = selectedMode();
    const bool hPattern = setup ? mode.forward != 0 && !mode.native : forward_ != 0 && !native_;
    const bool usesNumpad = hPattern || (setup
        ? std::ranges::any_of(savedRows_, [this](std::size_t row) { return isNumpadDigit(edited_[row].key); })
        : isNumpadDigit(key_) || std::ranges::any_of(draft_, [](const auto& binding) {
            return isNumpadDigit(binding.key);
        }));
    ShowWindow(GetDlgItem(window_, IDC_SHIFTER_NUMLOCK_HINT), usesNumpad ? SW_SHOW : SW_HIDE);
}

const ModeOption& ShifterDialog::selectedMode() const
{
    const auto row = send(IDC_SHIFTER_MODE, CB_GETCURSEL);
    if (row >= 0) {
        const auto index = static_cast<std::size_t>(send(IDC_SHIFTER_MODE, CB_GETITEMDATA, static_cast<WPARAM>(row)));
        if (index < modes.size())
            return modes[index];
    }
    return modes.front();
}

bool ShifterDialog::replacesSetup(const AccessoryBinding& binding) const
{
    if (native_)
        return isNativeTransmissionBinding(binding);
    return forward_ != 0 && binding.nativeShifter && binding.deviceId == deviceId_;
}

std::size_t ShifterDialog::initialMode() const
{
    unsigned int positions = 0;
    for (const auto& binding : edited_) {
        if (binding.nativeSequential)
            return 0;
        if (binding.nativeShifter)
            positions |= nativeShifterPositionBit(binding.role);
    }
    const auto forward = (positions & (1u << 8)) ? 8u : (positions & (1u << 7)) ? 7u : 6u;
    for (const auto& mode : modes) {
        if (mode.native && mode.forward == forward && positions != 0)
            return static_cast<std::size_t>(&mode - modes.data());
    }
    return 0;
}

const DeviceInfo* ShifterDialog::selectedDevice(bool attachedOnly) const
{
    const auto selected = send(IDC_SHIFTER_DEVICE, CB_GETCURSEL);
    if (selected == CB_ERR)
        return nullptr;
    const auto index = static_cast<std::size_t>(send(IDC_SHIFTER_DEVICE, CB_GETITEMDATA, static_cast<WPARAM>(selected)));
    return index < (attachedOnly ? attachedCount_ : devices_.size()) ? &devices_[index] : nullptr;
}

std::optional<std::size_t> ShifterDialog::selectedBinding() const
{
    const auto row = send(IDC_SHIFTER_LIST, LB_GETCURSEL);
    if (stage_ == Stage::Setup && row >= 0 && static_cast<std::size_t>(row) < savedRows_.size())
        return savedRows_[static_cast<std::size_t>(row)];
    return std::nullopt;
}

void ShifterDialog::showSavedBindings(LRESULT selection)
{
    const auto top = send(IDC_SHIFTER_LIST, LB_GETTOPINDEX);
    send(IDC_SHIFTER_LIST, LB_RESETCONTENT);
    savedRows_.clear();
    const auto* device = selectedDevice();
    if (!device)
        return;
    const bool nativeVerified = nativeShifterVerified(edited_);
    for (std::size_t i = 0; i < edited_.size(); ++i) {
        const auto& binding = edited_[i];
        if (binding.deviceId != device->id || binding.role == ShifterRole::None)
            continue;
        const auto label = bindingSummary(binding, nativeVerified);
        if (send(IDC_SHIFTER_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())) >= 0)
            savedRows_.push_back(i);
    }
    if (!savedRows_.empty() && selection >= 0) {
        const auto row = std::min(static_cast<std::size_t>(selection), savedRows_.size() - 1);
        send(IDC_SHIFTER_LIST, LB_SETCURSEL, static_cast<WPARAM>(row));
        if (top >= 0)
            send(IDC_SHIFTER_LIST, LB_SETTOPINDEX, static_cast<WPARAM>(top));
    }
}

void ShifterDialog::showPreview(std::wstring text)
{
    if (previewStatus_ != text) {
        previewStatus_ = std::move(text);
        SetDlgItemTextW(window_, IDC_SHIFTER_PREVIEW_STATUS, previewStatus_.c_str());
    }
}

void ShifterDialog::selectPreview(const AccessoryBinding* binding)
{
    const auto selected = binding ? std::optional{*binding} : std::nullopt;
    if (selected != previewBinding_) {
        previewBinding_ = selected;
        preview_.reset();
        previewIndex_ = 0;
        if (binding && !isNativeTransmissionBinding(*binding)) {
            preview_.emplace(AccessoryProfile{*binding});
        } else if (binding && (!binding->nativeShifter || nativeShifterVerified(edited_))) {
            AccessoryProfile native;
            for (const auto& position : edited_) {
                if (!isNativeTransmissionBinding(position))
                    continue;
                if (position == *binding)
                    previewIndex_ = native.size();
                native.push_back(position);
            }
            preview_.emplace(native);
        }
        if (preview_)
            showPreview(tr(Text::DlgAccessoryPreviewPending));
    }
    previewFocusChanged(GetForegroundWindow() == window_);
}

void ShifterDialog::previewFocusChanged(bool active)
{
    if (active && stage_ == Stage::Setup && preview_ && IsWindowEnabled(window_)) {
        if (!previewRunning_)
            previewRunning_ = SetTimer(window_, previewTimer, previewInterval, nullptr) != 0;
        return;
    }
    if (previewRunning_) {
        KillTimer(window_, previewTimer);
        previewRunning_ = false;
        if (preview_)
            preview_->reset();
    }
    if (stage_ != Stage::Setup)
        showPreview({});
    else if (!previewBinding_)
        showPreview(tr(Text::DlgShifterPreviewSelect));
    else if (!preview_)
        showPreview(tr(Text::DlgShifterNativeRecapture));
    else
        showPreview(tr(Text::DlgAccessoryPreviewPaused));
}

void ShifterDialog::pollPreview()
{
    if (!previewRunning_ || !preview_ || !inventory_.current())
        return;
    if (GetForegroundWindow() != window_ || !IsWindowEnabled(window_)) {
        previewFocusChanged(false);
        return;
    }
    std::array<AccessoryObservation, maxAccessoryBindings> observations;
    input_.beginPoll();
    preview_->poll(input_, observations);
    if (GetForegroundWindow() != window_ || !IsWindowEnabled(window_)) {
        previewFocusChanged(false);
        return;
    }

    const auto& observation = observations[previewIndex_];
    std::wstring message;
    switch (observation.status) {
    case AccessoryStatus::deviceUnavailable:
        message = tr(Text::DlgAccessoryPreviewDevice);
        break;
    case AccessoryStatus::inputUnavailable:
        message = tr(Text::DlgAccessoryPreviewInput);
        break;
    case AccessoryStatus::temporarilyUnavailable:
        message = tr(Text::DlgAccessoryPreviewPending);
        break;
    case AccessoryStatus::waitingForNeutral:
        message = tr(Text::DlgAccessoryPreviewNeutral);
        break;
    case AccessoryStatus::armed:
        if (isNativeTransmissionBinding(*previewBinding_)) {
            message = shifterRoleLabel(previewBinding_->role) + tr(observation.engaged.value_or(false)
                ? Text::DlgShifterPreviewEngaged : Text::DlgShifterPreviewReleased);
        } else {
            const bool pulse = previewBinding_->activation == AccessoryActivation::pulse;
            const auto output = pulse ? (observation.active ? Text::DlgAccessoryPreviewPulseActive
                                                           : Text::DlgAccessoryPreviewPulseReady)
                                      : (observation.active ? Text::DlgAccessoryPreviewPressed
                                                            : Text::DlgAccessoryPreviewReleased);
            message = accessoryKeyLabel(previewBinding_->key) + tr(output);
        }
        break;
    }
    if (observation.travel) {
        const auto percent = std::lround(std::clamp(*observation.travel, 0.0, 1.0) * 100.0);
        message += L"  ·  ";
        message += tr(Text::DlgAccessoryPreviewTravel) + std::to_wstring(percent) + L" %";
    }
    showPreview(std::move(message));
}

void ShifterDialog::selectDevice()
{
    if (stage_ != Stage::Setup)
        return;
    showSavedBindings();
    status(tr(setupStatus()));
    updateControls();
}

void ShifterDialog::selectMode()
{
    if (stage_ != Stage::Setup)
        return;
    status(tr(setupStatus()));
    updateControls();
}

bool ShifterDialog::legacyNativeMode() const
{
    const auto& mode = selectedMode();
    return mode.native && mode.forward > 7;
}

Text ShifterDialog::setupStatus() const
{
    if (legacyNativeMode())
        return Text::DlgShifterNativeH8Limit;
    if (!inventory_.current())
        return Text::mainScanningDevices;
    if (edited_ != bindings_)
        return Text::DlgShifterEditSave;
    if (devices_.empty())
        return Text::DlgShifterNoDevices;
    if (!selectedDevice(true))
        return Text::DlgShifterEditOffline;
    const auto& mode = selectedMode();
    const bool nativeOnDevice = std::ranges::any_of(savedRows_, [this](std::size_t row) {
        return edited_[row].nativeShifter;
    });
    if (mode.native && mode.forward == 0)
        return Text::DlgShifterNativeSequentialSetup;
    if (mode.native)
        return nativeOnDevice && !nativeShifterVerified(edited_)
            ? Text::DlgShifterNativeRecapture : Text::DlgShifterNativeSetup;
    if (mode.forward)
        return nativeOnDevice ? Text::DlgShifterReplaceNative : Text::DlgShifterExternalSetup;
    return savedRows_.empty() ? Text::DlgShifterNeutralThenStart : Text::DlgShifterEditOrStart;
}

void ShifterDialog::enterSetup()
{
    stage_ = Stage::Setup;
    setKey(0);
    SetDlgItemTextW(window_, IDC_SHIFTER_STEP, tr(Text::DlgShifterCapture));
    SetDlgItemTextW(window_, IDC_MODAL_LIST_HEADING, tr(Text::DlgShifterDeviceBindings));
    if (!refreshDevices())
        selectDevice();
}

void ShifterDialog::runTimer()
{
    deadline_ = GetTickCount64() + captureTimeout;
    if (SetTimer(window_, captureTimer, captureInterval, nullptr))
        return;
    enterSetup();
    status(tr(Text::DlgShifterMeasurementFailed));
}

void ShifterDialog::interruptCapture(Text analogMessage, Text positionMessage, Text neutralMessage)
{
    KillTimer(window_, captureTimer);
    if (stage_ == Stage::AnalogMeasure) {
        stage_ = Stage::AnalogReady;
        status(tr(analogMessage));
    } else if (stage_ == Stage::Neutral && analog_ && !draft_.empty()) {
        stage_ = Stage::AnalogReady;
        status(tr(Text::DlgShifterCaptureDeviceRest));
    } else if (stage_ == Stage::Position) {
        stage_ = Stage::Paused;
        status(tr(positionMessage));
    } else {
        enterSetup();
        status(tr(neutralMessage));
        return;
    }
    updateControls();
}

void ShifterDialog::start()
{
    if (hasCaptureChanges() && !confirm(Text::DlgShifterDiscardCapture))
        return;
    KillTimer(window_, captureTimer);
    draft_.clear();
    absentNativePositions_ = 0;
    pending_.reset();
    rest_.reset();
    digital_.reset();
    measurements_ = {};
    if (stage_ != Stage::Setup) {
        enterSetup();
        return;
    }
    refreshDevices();
    if (!inventory_.current()) {
        status(tr(Text::mainScanningDevices));
        return;
    }
    const auto* device = selectedDevice(true);
    if (!device || legacyNativeMode())
        return;
    const auto& mode = selectedMode();
    analog_ = mode.forward == 0 && send(IDC_SHIFTER_SIGNAL, CB_GETCURSEL) == 1;
    if (analog_ && std::ranges::none_of(device->axes, [](bool present) { return present; })) {
        status(tr(Text::DlgShifterNoAnalogAxis));
        return;
    }
    send(IDC_SHIFTER_LIST, LB_RESETCONTENT);
    SetDlgItemTextW(window_, IDC_MODAL_LIST_HEADING, tr(Text::DlgShifterCapturedList));
    deviceId_ = device->id;
    forward_ = mode.forward;
    native_ = mode.native;
    step_ = 0;
    if (!analog_)
        digital_.emplace(deviceId_);
    stage_ = Stage::Neutral;
    status(tr(Text::DlgShifterHoldNeutral));
    updateControls();
    runTimer();
}

ShifterRole ShifterDialog::role() const
{
    if (!forward_)
        return step_ == 0 ? ShifterRole::Upshift : ShifterRole::Downshift;
    return step_ == forward_ ? ShifterRole::Reverse
        : static_cast<ShifterRole>(static_cast<std::size_t>(ShifterRole::Gear1) + step_);
}

unsigned int ShifterDialog::suggestedKey() const
{
    if (!forward_)
        return step_ == 0 ? VK_ADD : VK_SUBTRACT;
    return step_ == forward_ ? VK_NUMPAD0 : VK_NUMPAD1 + static_cast<unsigned int>(step_);
}

void ShifterDialog::setKey(unsigned int key)
{
    key_ = key;
    const auto label = key_ ? accessoryKeyLabel(key_) + L"…" : std::wstring(tr(Text::DlgChooseKey));
    SetDlgItemTextW(window_, IDC_SHIFTER_KEY, label.c_str());
}

void ShifterDialog::chooseKey()
{
    if (native_ || (stage_ != Stage::Captured && stage_ != Stage::AnalogReady && stage_ != Stage::Paused))
        return;
    if (const auto key = chooseAccessoryKey(window_, key_)) {
        setKey(*key);
        updateNumLockHint();
    }
}

void ShifterDialog::showShortcut() const
{
    auto label = std::wstring(tr(Text::mainMenuTransmissionKey));
    if (const auto key = shortcut_.display())
        label += L" (" + accessoryKeyLabel(*key) + L")";
    label += L"…";
    SetDlgItemTextW(window_, IDC_SHIFTER_SHORTCUT, label.c_str());
    SetDlgItemTextW(window_, IDC_SHIFTER_SHORTCUT_HINT, tr(shortcut_.saved() ? Text::DlgShifterShortcutHint
        : Text::DlgShifterShortcutUnreadable));
    InvalidateRect(GetDlgItem(window_, IDC_SHIFTER_SHORTCUT), nullptr, FALSE);
}

void ShifterDialog::chooseShortcut()
{
    if (stage_ != Stage::Setup && stage_ != Stage::Done)
        return;
    editTransmissionShortcut(window_, profileRoot_, shortcut_);
    showShortcut();
}

void ShifterDialog::editSavedKey()
{
    const auto index = selectedBinding();
    if (!index || isNativeTransmissionBinding(edited_[*index]))
        return;
    const auto selection = send(IDC_SHIFTER_LIST, LB_GETCURSEL);
    switch (chooseBindingKey(window_, edited_, *index)) {
    case BindingKeyEdit::unchanged:
        break;
    case BindingKeyEdit::occupied:
        status(tr(Text::DlgAccessoriesKeyAlreadyAssigned));
        break;
    case BindingKeyEdit::handbrakeInputConflict:
        status(tr(Text::DlgHandbrakeInputConflict));
        break;
    case BindingKeyEdit::changed:
        showSavedBindings(selection);
        status(tr(setupStatus()));
        updateControls();
        break;
    }
}

void ShifterDialog::editSavedThresholds()
{
    const auto index = selectedBinding();
    if (!index)
        return;
    auto& binding = edited_[*index];
    if (binding.kind != AccessoryKind::Axis || isNativeTransmissionBinding(binding))
        return;
    if (const auto values = editAccessoryThresholds(window_, binding.thresholds);
        values && *values != binding.thresholds) {
        binding.thresholds = *values;
        showSavedBindings(send(IDC_SHIFTER_LIST, LB_GETCURSEL));
        status(tr(Text::DlgAccessoriesThresholdsEdited));
        updateControls();
    }
}

void ShifterDialog::removeSaved()
{
    const auto index = selectedBinding();
    if (!index)
        return;
    const bool native = isNativeTransmissionBinding(edited_[*index]);
    if (!confirm(native ? Text::DlgAccessoriesRemoveNative : Text::DlgShifterRemoveBinding))
        return;
    if (native)
        std::erase_if(edited_, isNativeTransmissionBinding);
    else
        edited_.erase(edited_.begin() + *index);
    showSavedBindings(send(IDC_SHIFTER_LIST, LB_GETCURSEL));
    status(tr(native ? Text::DlgAccessoriesNativeRemoved : Text::DlgAccessoriesBindingRemoved));
    updateControls();
}

void ShifterDialog::changeActivation()
{
    const auto selected = send(IDC_SHIFTER_ACTIVATION, CB_GETCURSEL);
    if (selected == CB_ERR)
        return;
    const auto activation = selected == 1 ? AccessoryActivation::pulse : AccessoryActivation::hold;
    if (stage_ == Stage::Captured && pending_) {
        if (!isNativeTransmissionBinding(*pending_))
            pending_->activation = activation;
        return;
    }
    const auto index = selectedBinding();
    if (!index)
        return;
    const auto row = send(IDC_SHIFTER_LIST, LB_GETCURSEL);
    auto& binding = edited_[*index];
    if (!isNativeTransmissionBinding(binding) && binding.activation != activation) {
        binding.activation = activation;
        showSavedBindings(row);
        status(tr(setupStatus()));
        updateControls();
    }
}

void ShifterDialog::position()
{
    KillTimer(window_, captureTimer);
    pending_.reset();
    if (step_ >= (forward_ ? forward_ + 1 : 2)) {
        stage_ = Stage::Done;
        setKey(0);
        SetDlgItemTextW(window_, IDC_SHIFTER_STEP, tr(Text::DlgShifterComplete));
        status(draft_.empty() ? tr(Text::DlgShifterNoPosition)
            : tr(native_ ? Text::DlgShifterSaveDirect : Text::DlgShifterExternalSaveDirect));
    } else {
        SetDlgItemTextW(window_, IDC_SHIFTER_STEP, shifterRoleLabel(role()).c_str());
        setKey(native_ ? 0 : suggestedKey());
        if (analog_) {
            stage_ = Stage::AnalogReady;
            status(tr(Text::DlgShifterHoldAndMeasure));
        } else {
            stage_ = Stage::Position;
            status(tr(native_ && forward_ ? Text::DlgShifterNativePosition : Text::DlgShifterNeutralThenHold));
            runTimer();
        }
    }
    updateControls();
}

void ShifterDialog::captured(AccessoryBinding binding)
{
    const auto existing = std::ranges::find_if(edited_, [&](const auto& other) {
        return !replacesSetup(other) && sameShifterInput(binding, other);
    });
    KillTimer(window_, captureTimer);
    if (native_ && existing != edited_.end()) {
        stage_ = analog_ ? Stage::AnalogReady : Stage::Paused;
        status(tr(Text::DlgShifterNativeInputConflict));
        updateControls();
        return;
    }
    if (existing != edited_.end())
        binding.activation = existing->activation;
    binding.nativeShifter = native_ && forward_ != 0;
    binding.nativeSequential = native_ && forward_ == 0;
    pending_ = std::move(binding);
    stage_ = Stage::Captured;
    status(tr(!native_ ? Text::DlgShifterInputDetected
                      : forward_ ? Text::DlgShifterNativeCaptured : Text::DlgShifterNativeSequentialCaptured));
    updateControls();
}

void ShifterDialog::poll()
{
    if (!IsWindowEnabled(window_) || (stage_ != Stage::Neutral && stage_ != Stage::Position && stage_ != Stage::AnalogMeasure))
        return;
    if (GetForegroundWindow() != window_) {
        interruptCapture(Text::DlgShifterMeasureInterrupted, Text::DlgShifterCaptureInterrupted,
                         Text::DlgShifterInitialCaptureInterrupted);
        return;
    }
    if (GetTickCount64() >= deadline_) {
        interruptCapture(Text::DlgShifterMeasureExpired, Text::DlgShifterCapturePaused,
                         Text::DlgShifterCaptureExpired);
        return;
    }
    const auto reading = readCaptureDevice(input_, deviceId_);
    const auto& sample = reading.sample;
    if (GetTickCount64() >= deadline_) {
        interruptCapture(Text::DlgShifterMeasureExpired, Text::DlgShifterCapturePaused,
                         Text::DlgShifterCaptureExpired);
        return;
    }
    if (!IsWindowEnabled(window_) || GetForegroundWindow() != window_) {
        interruptCapture(Text::DlgShifterMeasureInterrupted, Text::DlgShifterCaptureInterrupted,
                         Text::DlgShifterInitialCaptureInterrupted);
        return;
    }
    if (!sample || reading.needsRest()) {
        if (digital_)
            digital_->deviceInterrupted();
        measurements_ = {};
        if (analog_ && stage_ == Stage::AnalogMeasure && reading.needsRest()) {
            KillTimer(window_, captureTimer);
            rest_.reset();
            stage_ = Stage::AnalogReady;
            status(tr(Text::DlgShifterCaptureDeviceRest));
            updateControls();
            return;
        }
    }
    if (!sample) {
        status(tr(Text::DlgShifterDeviceReadInterrupted));
        return;
    }
    if (digital_) {
        if (stage_ == Stage::Neutral) {
            if (digital_->captureNeutral(*sample))
                position();
            else
                status(tr(Text::DlgShifterHoldNeutral));
            return;
        }
        const auto result = digital_->sample(*sample, draft_);
        switch (result.status) {
        case ShifterCaptureStatus::Captured:
            captured(*result.binding);
            break;
        case ShifterCaptureStatus::Ambiguous:
            status(tr(Text::DlgShifterMultipleInputs));
            break;
        case ShifterCaptureStatus::Duplicate:
            status(tr(Text::DlgShifterInputAlreadyUsed));
            break;
        case ShifterCaptureStatus::AwaitingNeutral:
            status(tr(Text::DlgShifterNeutralBeforeMeasure));
            break;
        case ShifterCaptureStatus::AwaitingInput:
            status(tr(Text::DlgShifterHoldUntilDetected));
            break;
        }
        return;
    }

    status(tr(stage_ == Stage::Neutral ? Text::DlgShifterHoldNeutral : Text::DlgShifterHoldAtLimit));
    if (!measurements_.add(*sample)) {
        measurements_ = {};
        return;
    }
    const auto measured = measurements_.result();
    if (!measured)
        return;
    if (stage_ == Stage::Neutral) {
        rest_ = measured;
        position();
        return;
    }
    const auto candidates = axisCandidates(*rest_, *rest_, *measured, false);
    if (candidates.size() == 1) {
        const auto index = candidates.front().axis;
        AccessoryBinding binding{deviceId_, AccessoryKind::Axis, index, rest_->median[index], measured->median[index], 0};
        if (std::ranges::none_of(draft_, [&](const auto& other) { return sameShifterInput(binding, other); })) {
            captured(std::move(binding));
            return;
        }
    }
    KillTimer(window_, captureTimer);
    stage_ = Stage::AnalogReady;
    status(tr(Text::DlgShifterSingleAxis));
    updateControls();
}

void ShifterDialog::advance()
{
    if (stage_ == Stage::Paused && digital_) {
        digital_->resetPosition();
        stage_ = Stage::Position;
        status(tr(Text::DlgShifterNeutralBeforeMeasure));
        updateControls();
        runTimer();
        return;
    }
    if (stage_ == Stage::AnalogReady) {
        measurements_ = {};
        stage_ = rest_ ? Stage::AnalogMeasure : Stage::Neutral;
        status(tr(rest_ ? Text::DlgShifterHoldAtLimit : Text::DlgShifterHoldNeutral));
        updateControls();
        runTimer();
        return;
    }
    if (stage_ != Stage::Captured || !pending_)
        return;
    if (!native_) {
        if (!isAccessoryKey(key_))
            return;
        if (std::ranges::find(draft_, key_, &AccessoryBinding::key) != draft_.end()) {
            status(tr(Text::DlgShifterKeyAlreadyUsed));
            return;
        }
        if (std::ranges::any_of(edited_, [&](const auto& other) {
                if (replacesSetup(other))
                    return false;
                const bool sameInput = sameShifterInput(*pending_, other);
                return sameInput != (other.key == key_);
            })) {
            status(tr(Text::DlgShifterBindingConflict));
            return;
        }
        pending_->key = key_;
    }
    pending_->role = role();
    const auto summary = bindingSummary(*pending_, true);
    send(IDC_SHIFTER_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(summary.c_str()));
    draft_.push_back(std::move(*pending_));
    ++step_;
    position();
}

void ShifterDialog::skip()
{
    if (forward_ == 0 || (stage_ != Stage::Position && stage_ != Stage::Paused &&
                          (native_ || stage_ != Stage::Captured)))
        return;
    if (native_) {
        // The modal confirmation must not consume the running measurement's time limit.
        KillTimer(window_, captureTimer);
        const auto question = shifterRoleLabel(role()) + L"\n\n" + tr(Text::DlgShifterNativeConfirmMissing);
        if (MessageBoxW(window_, question.c_str(), tr(Text::DlgShifterTitle),
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            if (stage_ == Stage::Position) {
                digital_->resetPosition();
                runTimer();
            }
            return;
        }
        absentNativePositions_ |= nativeShifterPositionBit(role());
        const auto summary = shifterRoleLabel(role()) + tr(Text::DlgShifterNativeMissingRow);
        send(IDC_SHIFTER_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(summary.c_str()));
    }
    digital_->resetPosition();
    ++step_;
    position();
}

void ShifterDialog::apply()
{
    const bool applyingCapture = stage_ == Stage::Done && !draft_.empty();
    if (!applyingCapture && ((stage_ != Stage::Setup && stage_ != Stage::Done) || edited_ == bindings_))
        return;
    auto updated = edited_;
    if (applyingCapture) {
        unsigned int presentPositions = 0;
        if (native_ && forward_) {
            for (const auto& binding : draft_)
                presentPositions |= nativeShifterPositionBit(binding.role);
            const unsigned int expected = (1u << (forward_ + 1)) - 1u;
            if ((presentPositions | absentNativePositions_) != expected || (presentPositions & absentNativePositions_) != 0) {
                status(tr(Text::DlgShifterNativeIncomplete));
                return;
            }
        }
        std::erase_if(updated, [this](const AccessoryBinding& binding) { return replacesSetup(binding); });
        for (auto binding : draft_) {
            if (binding.nativeShifter)
                binding.nativeShifterPositions = presentPositions;
            const auto existing = isNativeTransmissionBinding(binding) ? updated.end()
                : std::ranges::find(updated, binding.key, &AccessoryBinding::key);
            if (existing == updated.end())
                updated.push_back(std::move(binding));
            else {
                const auto thresholds = existing->thresholds;
                *existing = binding;
                existing->thresholds = thresholds;
            }
        }
    }
    if (updated.size() > maxAccessoryBindings) {
        status(tr(Text::DlgShifterBindingLimit));
        return;
    }
    if (!save_(window_, updated))
        return;
    bindings_ = std::move(updated);
    EndDialog(window_, IDOK);
}

bool ShifterDialog::confirm(Text message) const
{
    return MessageBoxW(window_, tr(message),
        tr(Text::DlgShifterTitle), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

void ShifterDialog::cancel()
{
    if ((!hasCaptureChanges() && edited_ == bindings_) || confirm(Text::DlgShifterDiscard))
        EndDialog(window_, IDCANCEL);
}

INT_PTR CALLBACK shifterProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG) {
        auto& dialog = *reinterpret_cast<ShifterDialog*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        SetWindowTextW(window, tr(Text::DlgShifterTitle));
        SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(Text::DlgShifterIntro));
        SetDlgItemTextW(window, IDC_MODAL_DEVICE_LABEL, tr(Text::DlgDevice));
        SetDlgItemTextW(window, IDC_SHIFTER_MODE_LABEL, tr(Text::DlgShifterMode));
        SetDlgItemTextW(window, IDC_SHIFTER_SIGNAL_LABEL, tr(Text::DlgShifterSignal));
        SetDlgItemTextW(window, IDC_SHIFTER_STEP, tr(Text::DlgShifterCapture));
        SetDlgItemTextW(window, IDC_MODAL_KEY_LABEL, tr(Text::DlgKey));
        SetDlgItemTextW(window, IDC_SHIFTER_KEY, tr(Text::DlgChooseKey));
        SetDlgItemTextW(window, IDC_SHIFTER_NUMLOCK_HINT, tr(Text::DlgKeypadNumLockHint));
        SetDlgItemTextW(window, IDC_MODAL_LIST_HEADING, tr(Text::DlgShifterDeviceBindings));
        SetDlgItemTextW(window, IDC_SHIFTER_ACTIVATION_LABEL, tr(Text::DlgAccessoryActivation));
        SetDlgItemTextW(window, IDC_SHIFTER_SKIP, tr(Text::DlgShifterSkip));
        SetDlgItemTextW(window, IDC_SHIFTER_EDIT, tr(Text::DlgBindingEdit));
        SetDlgItemTextW(window, IDC_SHIFTER_REMOVE, tr(Text::DlgShifterRemove));
        SetDlgItemTextW(window, IDC_SHIFTER_THRESHOLDS, tr(Text::DlgShifterThresholds));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, tr(Text::DlgShifterHeading), IDC_SHIFTER_STATUS, IDC_SHIFTER_LIST,
                            {IDC_SHIFTER_START, IDC_SHIFTER_ADVANCE, IDOK});
        dialog.initialise(window);
        return TRUE;
    }
    auto* dialog = reinterpret_cast<ShifterDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog)
        return FALSE;
    if (message == WM_TIMER) {
        if (wp == captureTimer) {
            dialog->poll();
            return TRUE;
        }
        if (wp == previewTimer) {
            dialog->pollPreview();
            return TRUE;
        }
        if (wp == inventoryTimer) {
            dialog->refreshDevices();
            return TRUE;
        }
    }
    if (message == WM_DEVICECHANGE &&
        (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)) {
        dialog->deviceTopologyChanged();
        return FALSE;
    }
    if (message == WM_ACTIVATE) {
        const bool active = LOWORD(wp) != WA_INACTIVE;
        if (active)
            dialog->refreshDevices();
        dialog->previewFocusChanged(active);
        return FALSE;
    }
    if (message == WM_CLOSE) {
        dialog->cancel();
        return TRUE;
    }
    if (message != WM_COMMAND)
        return FALSE;
    switch (LOWORD(wp)) {
    case IDC_SHIFTER_DEVICE:
        if (HIWORD(wp) == CBN_SELCHANGE)
            dialog->selectDevice();
        return TRUE;
    case IDC_SHIFTER_MODE:
        if (HIWORD(wp) == CBN_SELCHANGE)
            dialog->selectMode();
        return TRUE;
    case IDC_SHIFTER_ACTIVATION:
        if (HIWORD(wp) == CBN_SELCHANGE)
            dialog->changeActivation();
        return TRUE;
    case IDC_SHIFTER_LIST:
        if (HIWORD(wp) == LBN_DBLCLK)
            dialog->editSavedKey();
        else if (HIWORD(wp) == LBN_SELCHANGE)
            dialog->updateControls();
        return TRUE;
    case IDC_SHIFTER_EDIT:
        dialog->editSavedKey();
        return TRUE;
    case IDC_SHIFTER_REMOVE:
        dialog->removeSaved();
        return TRUE;
    case IDC_SHIFTER_THRESHOLDS:
        dialog->editSavedThresholds();
        return TRUE;
    case IDC_SHIFTER_START:
        dialog->start();
        return TRUE;
    case IDC_SHIFTER_KEY:
        if (HIWORD(wp) == BN_CLICKED)
            dialog->chooseKey();
        return TRUE;
    case IDC_SHIFTER_SHORTCUT:
        if (HIWORD(wp) == BN_CLICKED)
            dialog->chooseShortcut();
        return TRUE;
    case IDC_SHIFTER_ADVANCE:
        dialog->advance();
        return TRUE;
    case IDC_SHIFTER_SKIP:
        dialog->skip();
        return TRUE;
    case IDOK:
        dialog->apply();
        return TRUE;
    case IDCANCEL:
        dialog->cancel();
        return TRUE;
    }
    return FALSE;
}

} // namespace

bool editTransmissionShortcut(HWND owner, const std::filesystem::path& root, TransmissionShortcut& shortcut)
{
    if (isTemporaryProfileDirectory(root)) {
        MessageBoxW(owner, tr(Text::DlgAccessoriesExtractBeforeSaving), tr(Text::mainTransmissionKeyTitle),
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    shortcut.refresh(root);
    const auto choice = chooseTransmissionKey(owner, shortcut.display().value_or(0));
    if (!choice || shortcut.saved() == choice)
        return false;
    std::string error;
    if (!saveInstallationSettings(root, [&] { return saveTransmissionHotkey(root, *choice, error); })) {
        const auto message = std::wstring(tr(Text::mainTransmissionKeySaveFailed)) + L"\n" + localizedError(error);
        MessageBoxW(owner, message.c_str(), tr(Text::mainTransmissionKeyTitle), MB_OK | MB_ICONERROR);
        return false;
    }
    shortcut.refresh(root, choice);
    return true;
}

bool editShifter(HWND owner, DirectInput& input, DeviceInventory& inventory, AccessoryProfile& bindings,
                 const std::filesystem::path& profileRoot, TransmissionShortcut& shortcut,
                 std::function<bool(HWND, const AccessoryProfile&)> save)
{
    ShifterDialog dialog(input, inventory, bindings, profileRoot, shortcut, std::move(save));
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SHIFTER), owner,
        shifterProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgShifterOpenError), tr(Text::DlgShifterTitle), MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
