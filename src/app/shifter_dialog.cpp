#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"

#include "chrome_axis/calibration.hpp"
#include "chrome_axis/keyboard_dialog.hpp"
#include "chrome_axis/shifter.hpp"

#include <algorithm>
#include <utility>

namespace chrome_axis {
namespace {

constexpr UINT_PTR captureTimer = 1;
constexpr UINT captureInterval = 20;
constexpr ULONGLONG captureTimeout = 30000;

enum class Stage { Setup, Neutral, Position, Paused, AnalogReady, AnalogMeasure, Captured, Done };

class ShifterDialog {
public:
    ShifterDialog(DirectInput& input, AccessoryProfile& bindings, std::string_view preferredDevice)
        : input_(input), bindings_(bindings), preferredDevice_(preferredDevice), edited_(bindings) {}

    void initialise(HWND window);
    void start();
    void poll();
    void advance();
    void skip();
    void updateControls();
    void selectDevice();
    void chooseKey();
    void editSavedKey();
    void apply();
    void cancel();

private:
    LRESULT send(int control, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) const
    {
        return SendDlgItemMessageW(window_, control, message, wParam, lParam);
    }
    void status(const wchar_t* text);
    void position();
    void captured(AccessoryBinding binding);
    bool runTimer();
    bool hasCaptureChanges() const { return !draft_.empty() || pending_.has_value(); }
    bool confirmDiscard(Text message) const;
    std::wstring role() const;
    unsigned int suggestedKey() const;
    void setKey(unsigned int key);
    void enterSetup();
    void showSavedBindings(LRESULT selection = LB_ERR);
    Text setupStatus() const;
    const DeviceInfo* selectedDevice(bool attachedOnly = false) const;
    bool sequential() const { return send(IDC_SHIFTER_MODE, CB_GETCURSEL) == 0; }

    DirectInput& input_;
    AccessoryProfile& bindings_;
    std::string preferredDevice_;
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
    Stage stage_{Stage::Setup};
    ULONGLONG deadline_{};
    bool analog_{};
};

void ShifterDialog::initialise(HWND window)
{
    window_ = window;
    devices_ = input_.devices();
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
        if (device.id == preferredDevice_)
            selection = static_cast<WPARAM>(row);
    }
    constexpr std::array modes{Text::DlgShifterSequential, Text::DlgShifterH6, Text::DlgShifterH7, Text::DlgShifterH8};
    for (const auto mode : modes)
        send(IDC_SHIFTER_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(mode)));
    for (const auto* signal : {tr(Text::DlgDigital), tr(Text::DlgAnalog)})
        send(IDC_SHIFTER_SIGNAL, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(signal));
    send(IDC_SHIFTER_DEVICE, CB_SETCURSEL, selection);
    send(IDC_SHIFTER_MODE, CB_SETCURSEL, 0);
    send(IDC_SHIFTER_SIGNAL, CB_SETCURSEL, 0);
    selectDevice();
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
    const bool canAdvance = stage_ == Stage::Captured || stage_ == Stage::AnalogReady || stage_ == Stage::Paused;
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_DEVICE), setup && !devices_.empty());
    const bool canCapture = selectedDevice(true) != nullptr;
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_MODE), setup && canCapture);
    if (setup && !sequential())
        send(IDC_SHIFTER_SIGNAL, CB_SETCURSEL, 0);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_SIGNAL), setup && canCapture && sequential());
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_START), !setup || canCapture);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_ADVANCE), canAdvance);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_KEY), stage_ == Stage::Captured ||
        stage_ == Stage::AnalogReady || stage_ == Stage::Paused);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_SKIP), forward_ != 0 &&
        (stage_ == Stage::Position || stage_ == Stage::Paused || stage_ == Stage::Captured));
    ShowWindow(GetDlgItem(window_, IDC_SHIFTER_SKIP), setup ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(window_, IDC_SHIFTER_EDIT), setup ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(window_, IDC_SHIFTER_EDIT), setup && send(IDC_SHIFTER_LIST, LB_GETCURSEL) >= 0);
    EnableWindow(GetDlgItem(window_, IDOK), (setup && edited_ != bindings_) ||
        (stage_ == Stage::Done && (!draft_.empty() || edited_ != bindings_)));
    SetDlgItemTextW(window_, IDC_SHIFTER_START, setup ? tr(Text::DlgShifterStart) : tr(Text::DlgShifterRestart));
    SetDlgItemTextW(window_, IDC_SHIFTER_ADVANCE, stage_ == Stage::AnalogReady ? tr(Text::DlgShifterMeasure)
        : stage_ == Stage::Paused ? tr(Text::DlgShifterResume) : tr(Text::DlgShifterNext));
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
    // Legacy profiles store physical inputs and keys without a gear or paddle role.
    for (std::size_t i = 0; i < edited_.size(); ++i) {
        const auto& binding = edited_[i];
        if (binding.deviceId != device->id)
            continue;
        const auto label = accessoryInputLabel(binding) + L"  →  " + accessoryKeyLabel(binding.key);
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

Text ShifterDialog::setupStatus() const
{
    if (edited_ != bindings_)
        return Text::DlgShifterEditApply;
    if (devices_.empty())
        return Text::DlgShifterNoDevices;
    if (!selectedDevice(true))
        return Text::DlgShifterEditOffline;
    return savedRows_.empty() ? Text::DlgShifterNeutralThenStart : Text::DlgShifterEditOrStart;
}

void ShifterDialog::enterSetup()
{
    stage_ = Stage::Setup;
    setKey(0);
    SetDlgItemTextW(window_, IDC_SHIFTER_STEP, tr(Text::DlgShifterCapture));
    SetDlgItemTextW(window_, IDC_MODAL_LIST_HEADING, tr(Text::DlgShifterDeviceBindings));
    selectDevice();
}

bool ShifterDialog::runTimer()
{
    deadline_ = GetTickCount64() + captureTimeout;
    if (SetTimer(window_, captureTimer, captureInterval, nullptr))
        return true;
    enterSetup();
    status(tr(Text::DlgShifterMeasurementFailed));
    return false;
}

void ShifterDialog::start()
{
    if (hasCaptureChanges() && !confirmDiscard(Text::DlgShifterDiscardCapture))
        return;
    KillTimer(window_, captureTimer);
    draft_.clear();
    pending_.reset();
    rest_.reset();
    digital_.reset();
    measurements_ = {};
    if (stage_ != Stage::Setup) {
        enterSetup();
        return;
    }
    const auto* device = selectedDevice(true);
    if (!device)
        return;
    analog_ = sequential() && send(IDC_SHIFTER_SIGNAL, CB_GETCURSEL) == 1;
    if (analog_ && std::ranges::none_of(device->axes, [](bool present) { return present; })) {
        status(tr(Text::DlgShifterNoAnalogAxis));
        return;
    }
    send(IDC_SHIFTER_LIST, LB_RESETCONTENT);
    SetDlgItemTextW(window_, IDC_MODAL_LIST_HEADING, tr(Text::DlgShifterCapturedList));
    deviceId_ = device->id;
    forward_ = sequential() ? 0 : static_cast<std::size_t>(send(IDC_SHIFTER_MODE, CB_GETCURSEL)) + 5;
    step_ = 0;
    if (!analog_)
        digital_.emplace(deviceId_);
    stage_ = Stage::Neutral;
    status(tr(Text::DlgShifterHoldNeutral));
    updateControls();
    runTimer();
}

std::wstring ShifterDialog::role() const
{
    if (!forward_)
        return step_ == 0 ? tr(Text::DlgShifterUpshift) : tr(Text::DlgShifterDownshift);
    return step_ == forward_ ? tr(Text::DlgShifterReverse) : tr(Text::DlgShifterGear) + std::to_wstring(step_ + 1);
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
    if (stage_ != Stage::Captured && stage_ != Stage::AnalogReady && stage_ != Stage::Paused)
        return;
    if (const auto key = chooseAccessoryKey(window_, key_))
        setKey(*key);
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
    case BindingKeyEdit::changed:
        showSavedBindings(selection);
        status(tr(setupStatus()));
        updateControls();
        break;
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
            : tr(Text::DlgShifterSaveAndConfigure));
    } else {
        SetDlgItemTextW(window_, IDC_SHIFTER_STEP, role().c_str());
        setKey(suggestedKey());
        if (analog_) {
            stage_ = Stage::AnalogReady;
            status(tr(Text::DlgShifterHoldAndMeasure));
        } else {
            stage_ = Stage::Position;
            status(tr(Text::DlgShifterNeutralThenHold));
            runTimer();
        }
    }
    updateControls();
}

void ShifterDialog::captured(AccessoryBinding binding)
{
    pending_ = std::move(binding);
    stage_ = Stage::Captured;
    KillTimer(window_, captureTimer);
    status(tr(Text::DlgShifterInputDetected));
    updateControls();
}

void ShifterDialog::poll()
{
    if (!IsWindowEnabled(window_) || (stage_ != Stage::Neutral && stage_ != Stage::Position && stage_ != Stage::AnalogMeasure))
        return;
    if (GetForegroundWindow() != window_) {
        KillTimer(window_, captureTimer);
        if (stage_ == Stage::AnalogMeasure) {
            stage_ = Stage::AnalogReady;
            status(tr(Text::DlgShifterMeasureInterrupted));
        } else if (stage_ == Stage::Position) {
            stage_ = Stage::Paused;
            status(tr(Text::DlgShifterCaptureInterrupted));
        } else {
            enterSetup();
            status(tr(Text::DlgShifterInitialCaptureInterrupted));
        }
        updateControls();
        return;
    }
    if (GetTickCount64() >= deadline_) {
        KillTimer(window_, captureTimer);
        if (stage_ == Stage::AnalogMeasure) {
            stage_ = Stage::AnalogReady;
            status(tr(Text::DlgShifterMeasureExpired));
        } else if (stage_ == Stage::Position) {
            stage_ = Stage::Paused;
            status(tr(Text::DlgShifterCapturePaused));
        } else {
            enterSetup();
            status(tr(Text::DlgShifterCaptureExpired));
        }
        updateControls();
        return;
    }
    const auto sample = input_.sample(deviceId_);
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
    if (stage_ == Stage::Paused) {
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
    const auto key = key_;
    if (!isAccessoryKey(key))
        return;
    if (std::ranges::find(draft_, key, &AccessoryBinding::key) != draft_.end()) {
        status(tr(Text::DlgShifterKeyAlreadyUsed));
        return;
    }
    if (std::ranges::any_of(edited_, [&](const auto& other) {
            const bool sameInput = sameShifterInput(*pending_, other);
            return sameInput != (other.key == key);
        })) {
        status(tr(Text::DlgShifterBindingConflict));
        return;
    }
    pending_->key = key;
    const auto summary = role() + L"  →  " + accessoryKeyLabel(key);
    send(IDC_SHIFTER_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(summary.c_str()));
    draft_.push_back(std::move(*pending_));
    ++step_;
    position();
}

void ShifterDialog::skip()
{
    if (forward_ && (stage_ == Stage::Position || stage_ == Stage::Paused || stage_ == Stage::Captured)) {
        digital_->resetPosition();
        ++step_;
        position();
    }
}

void ShifterDialog::apply()
{
    const bool applyingCapture = stage_ == Stage::Done && !draft_.empty();
    if (!applyingCapture && ((stage_ != Stage::Setup && stage_ != Stage::Done) || edited_ == bindings_))
        return;
    auto updated = edited_;
    if (applyingCapture) {
        for (const auto& binding : draft_) {
            const auto existing = std::ranges::find(updated, binding.key, &AccessoryBinding::key);
            if (existing == updated.end())
                updated.push_back(binding);
            else
                *existing = binding;
        }
    }
    if (updated.size() > maxAccessoryBindings) {
        status(tr(Text::DlgShifterBindingLimit));
        return;
    }
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
        SetDlgItemTextW(window, IDC_MODAL_LIST_HEADING, tr(Text::DlgShifterDeviceBindings));
        SetDlgItemTextW(window, IDC_SHIFTER_SKIP, tr(Text::DlgShifterSkip));
        SetDlgItemTextW(window, IDC_SHIFTER_EDIT, tr(Text::DlgBindingEdit));
        SetDlgItemTextW(window, IDOK, tr(Text::DlgShifterApply));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, tr(Text::DlgShifterHeading), IDC_SHIFTER_STATUS, IDC_SHIFTER_LIST,
                            {IDC_SHIFTER_START, IDC_SHIFTER_ADVANCE, IDOK});
        dialog.initialise(window);
        return TRUE;
    }
    auto* dialog = reinterpret_cast<ShifterDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog)
        return FALSE;
    if (message == WM_TIMER && wp == captureTimer) {
        dialog->poll();
        return TRUE;
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
            dialog->updateControls();
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

bool editShifter(HWND owner, DirectInput& input, AccessoryProfile& bindings, std::string_view preferredDevice)
{
    ShifterDialog dialog(input, bindings, preferredDevice);
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SHIFTER), owner,
        shifterProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgShifterOpenError), tr(Text::DlgShifterTitle), MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
