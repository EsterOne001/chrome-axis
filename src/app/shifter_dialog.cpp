#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"

#include "chrome_axis/calibration.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/keyboard_dialog.hpp"
#include "chrome_axis/shifter.hpp"

#include <dbt.h>

#include <algorithm>
#include <array>
#include <utility>

namespace chrome_axis {
namespace {

constexpr UINT_PTR captureTimer = 1;
constexpr UINT captureInterval = 20;
constexpr ULONGLONG captureTimeout = 30000;
constexpr UINT_PTR inventoryTimer = 2;
constexpr UINT inventoryDebounceMs = 300;

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
    ShifterDialog(DirectInput& input, AccessoryProfile& bindings, std::span<const DeviceInfo> devices,
                  std::string_view preferredDevice, const std::filesystem::path& profileRoot,
                  std::function<bool(HWND, const AccessoryProfile&)> save)
        : input_(input), bindings_(bindings), preferredDevice_(preferredDevice), profileRoot_(profileRoot), save_(std::move(save)),
          devices_(devices.begin(), devices.end()), edited_(bindings) {}

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
    bool confirmDiscard(Text message) const;
    ShifterRole role() const;
    unsigned int suggestedKey() const;
    void setKey(unsigned int key);
    void enterSetup();
    void showSavedBindings(LRESULT selection = LB_ERR);
    void fillDevices(std::string_view preferredDevice);
    Text setupStatus() const;
    const DeviceInfo* selectedDevice(bool attachedOnly = false) const;
    const ModeOption& selectedMode() const;
    bool replacesSetup(const AccessoryBinding& binding) const;
    std::size_t initialMode() const;
    void showShortcut(unsigned int shortcut) const;

    DirectInput& input_;
    AccessoryProfile& bindings_;
    std::string preferredDevice_;
    const std::filesystem::path& profileRoot_;
    std::function<bool(HWND, const AccessoryProfile&)> save_;
    HWND window_{};
    std::vector<DeviceInfo> devices_;
    std::size_t attachedCount_{};
    AccessoryProfile edited_;
    std::vector<std::size_t> savedRows_;
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
    bool inventoryDirty_{};
};

void ShifterDialog::initialise(HWND window)
{
    window_ = window;
    showShortcut(loadTransmissionHotkey(profileRoot_));
    SetDlgItemTextW(window_, IDC_SHIFTER_SHORTCUT_HINT, tr(Text::DlgShifterShortcutHint));
    SetDlgItemTextW(window_, IDOK, tr(save_ ? Text::DlgSave : Text::DlgShifterApply));
    if (preferredDevice_.empty()) {
        const auto saved = std::ranges::find_if(edited_, isNativeTransmissionBinding);
        if (saved != edited_.end())
            preferredDevice_ = saved->deviceId;
    }
    fillDevices(preferredDevice_);
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const auto row = send(IDC_SHIFTER_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(modes[i].label)));
        if (row >= 0 && send(IDC_SHIFTER_MODE, CB_SETITEMDATA, static_cast<WPARAM>(row),
                             static_cast<LPARAM>(i)) == CB_ERR)
            send(IDC_SHIFTER_MODE, CB_DELETESTRING, static_cast<WPARAM>(row));
    }
    for (const auto* signal : {tr(Text::DlgDigital), tr(Text::DlgAnalog)})
        send(IDC_SHIFTER_SIGNAL, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(signal));
    populateAccessoryActivation(window_, IDC_SHIFTER_ACTIVATION);
    send(IDC_SHIFTER_MODE, CB_SETCURSEL, initialMode());
    send(IDC_SHIFTER_SIGNAL, CB_SETCURSEL, 0);
    selectDevice();
}

void ShifterDialog::fillDevices(std::string_view preferredDevice)
{
    send(IDC_SHIFTER_DEVICE, CB_RESETCONTENT);
    attachedCount_ = devices_.size();
    for (const auto& binding : edited_) {
        if (std::ranges::find(devices_, binding.deviceId, &DeviceInfo::id) == devices_.end()) {
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
    inventoryDirty_ = true;
    if (!SetTimer(window_, inventoryTimer, inventoryDebounceMs, nullptr))
        refreshDevices();
}

bool ShifterDialog::refreshDevices()
{
    KillTimer(window_, inventoryTimer);
    if (!inventoryDirty_ || stage_ != Stage::Setup || !IsWindowEnabled(window_))
        return false;
    const auto* selected = selectedDevice();
    const auto preferred = selected ? selected->id : preferredDevice_;
    inventoryDirty_ = false;
    devices_ = input_.devices();
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
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_START), !setup || canCapture);
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
    const auto row = send(IDC_SHIFTER_LIST, LB_GETCURSEL);
    const bool selected = setup && row >= 0 && static_cast<std::size_t>(row) < savedRows_.size();
    const auto* saved = selected ? &edited_[savedRows_[static_cast<std::size_t>(row)]] : nullptr;
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_EDIT), saved && !isNativeTransmissionBinding(*saved));
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
        if (binding.deviceId != device->id || binding.handbrakeCeilingPercent || binding.clutchMode)
            continue;
        const auto label = bindingSummary(binding, nativeVerified);
        if (send(IDC_SHIFTER_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())) >= 0)
            savedRows_.push_back(i);
    }
    if (selection >= 0) {
        send(IDC_SHIFTER_LIST, LB_SETCURSEL, static_cast<WPARAM>(selection));
        if (top >= 0)
            send(IDC_SHIFTER_LIST, LB_SETTOPINDEX, static_cast<WPARAM>(top));
    }
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

Text ShifterDialog::setupStatus() const
{
    if (devices_.empty())
        return Text::DlgShifterNoDevices;
    if (!selectedDevice(true))
        return edited_ != bindings_ ? (save_ ? Text::DlgShifterEditSave : Text::DlgShifterEditApply)
                                    : Text::DlgShifterEditOffline;
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
    if (edited_ != bindings_)
        return save_ ? Text::DlgShifterEditSave : Text::DlgShifterEditApply;
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
    if (hasCaptureChanges() && !confirmDiscard(Text::DlgShifterDiscardCapture))
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
    const auto* device = selectedDevice(true);
    if (!device)
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

void ShifterDialog::showShortcut(unsigned int shortcut) const
{
    const auto label = std::wstring(tr(Text::mainMenuTransmissionKey)) + L" (" + accessoryKeyLabel(shortcut) + L")…";
    SetDlgItemTextW(window_, IDC_SHIFTER_SHORTCUT, label.c_str());
    InvalidateRect(GetDlgItem(window_, IDC_SHIFTER_SHORTCUT), nullptr, FALSE);
}

void ShifterDialog::chooseShortcut()
{
    if (stage_ != Stage::Setup && stage_ != Stage::Done)
        return;
    if (isTemporaryProfileDirectory(profileRoot_)) {
        MessageBoxW(window_, tr(Text::DlgAccessoriesExtractBeforeSaving), tr(Text::mainTransmissionKeyTitle),
                    MB_OK | MB_ICONWARNING);
        return;
    }
    const auto current = loadTransmissionHotkey(profileRoot_);
    const auto choice = chooseTransmissionKey(window_, current);
    if (!choice || *choice == current)
        return;
    std::string error;
    if (!saveTransmissionHotkey(profileRoot_, *choice, error)) {
        const auto message = std::wstring(tr(Text::mainTransmissionKeySaveFailed)) + localizedError(error);
        MessageBoxW(window_, message.c_str(), tr(Text::mainTransmissionKeyTitle), MB_OK | MB_ICONERROR);
        return;
    }
    showShortcut(*choice);
}

void ShifterDialog::editSavedKey()
{
    if (stage_ != Stage::Setup)
        return;
    const auto selection = send(IDC_SHIFTER_LIST, LB_GETCURSEL);
    if (selection < 0 || static_cast<std::size_t>(selection) >= savedRows_.size())
        return;
    switch (chooseBindingKey(window_, edited_, savedRows_[static_cast<std::size_t>(selection)])) {
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
    if (stage_ != Stage::Setup)
        return;
    const auto row = send(IDC_SHIFTER_LIST, LB_GETCURSEL);
    if (row < 0 || static_cast<std::size_t>(row) >= savedRows_.size())
        return;
    auto& binding = edited_[savedRows_[static_cast<std::size_t>(row)]];
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
            : tr(native_ ? (save_ ? Text::DlgShifterSaveDirect : Text::DlgShifterNativeSave)
                         : (save_ ? Text::DlgShifterExternalSaveDirect : Text::DlgShifterSaveAndConfigure)));
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
    input_.beginPoll();
    const auto sample = input_.sample(deviceId_);
    if (GetTickCount64() >= deadline_) {
        interruptCapture(Text::DlgShifterMeasureExpired, Text::DlgShifterCapturePaused,
                         Text::DlgShifterCaptureExpired);
        return;
    }
    if (!sample) {
        if (digital_ && stage_ == Stage::Neutral)
            digital_.emplace(deviceId_);
        else if (digital_)
            digital_->resetPosition();
        measurements_ = {};
        status(tr(Text::DlgShifterDeviceReadInterrupted));
        return;
    }
    if (digital_) {
        if (stage_ == Stage::Neutral) {
            if (digital_->captureNeutral(*sample))
                position();
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
        stage_ = Stage::AnalogMeasure;
        status(tr(Text::DlgShifterHoldAtLimit));
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
    if (save_ && !save_(window_, updated))
        return;
    bindings_ = std::move(updated);
    EndDialog(window_, IDOK);
}

bool ShifterDialog::confirmDiscard(Text message) const
{
    return MessageBoxW(window_, tr(message),
        tr(Text::DlgShifterTitle), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

void ShifterDialog::cancel()
{
    if ((!hasCaptureChanges() && edited_ == bindings_) || confirmDiscard(Text::DlgShifterDiscard))
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
    if (message == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) {
        dialog->refreshDevices();
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

bool editShifter(HWND owner, DirectInput& input, AccessoryProfile& bindings,
                 std::span<const DeviceInfo> devices, std::string_view preferredDevice,
                 const std::filesystem::path& profileRoot, std::function<bool(HWND, const AccessoryProfile&)> save)
{
    ShifterDialog dialog(input, bindings, devices, preferredDevice, profileRoot, std::move(save));
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SHIFTER), owner,
        shifterProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgShifterOpenError), tr(Text::DlgShifterTitle), MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
