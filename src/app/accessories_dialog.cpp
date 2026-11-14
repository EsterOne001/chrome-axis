#include "chrome_axis/accessories_dialog.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"
#include "ui_theme.hpp"

#include "chrome_axis/accessories.hpp"
#include "chrome_axis/calibration.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/keyboard_dialog.hpp"
#include "chrome_axis/presets.hpp"

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
    const auto key = chooseAccessoryKey(owner, binding.key);
    if (!key || *key == binding.key)
        return BindingKeyEdit::unchanged;
    if (std::ranges::find(bindings, *key, &AccessoryBinding::key) != bindings.end())
        return BindingKeyEdit::occupied;
    binding.key = *key;
    return BindingKeyEdit::changed;
}

namespace {

constexpr UINT_PTR captureTimer = 1;
constexpr UINT captureIntervalMs = 30;
constexpr ULONGLONG captureTimeoutMs = 12000;
constexpr UINT_PTR previewTimer = 2;
constexpr UINT previewIntervalMs = 50;

std::wstring percent(int value)
{
    return std::to_wstring(value) + tr(Text::mainPercentSuffix);
}

enum class CaptureStage { Idle, Digital, AxisRest, AwaitingTravel, AxisTravel };

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

namespace {

std::optional<AccessoryThresholds> readThresholds(HWND window)
{
    const auto readPercent = [window](int control) {
        wchar_t text[5]{};
        const auto length = GetDlgItemTextW(window, control, text, static_cast<int>(std::size(text)));
        if (!length || length > 3)
            return 0u;
        unsigned int value = 0;
        for (UINT i = 0; i < length; ++i) {
            if (text[i] < L'0' || text[i] > L'9')
                return 0u;
            value = value * 10 + static_cast<unsigned int>(text[i] - L'0');
        }
        return value;
    };
    const AccessoryThresholds values{readPercent(IDC_THRESHOLDS_PRESS), readPercent(IDC_THRESHOLDS_RELEASE)};
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

class AccessoryDialog {
public:
    AccessoryDialog(DirectInput& input, const std::filesystem::path& root, const ProfilePreset& preset)
        : input_(input), root_(root), preset_(preset), path_(preset.directory / accessoryFilename) {}

    bool initialise(HWND window);
    void capture();
    void sample();
    void refreshDevices();
    void clearCapture();
    void refreshActions();
    void selectPreview();
    void setPreviewFocus(bool focused);
    void pollPreview();
    void drawPreview(const DRAWITEMSTRUCT& item) const;
    void selectCapturedInput();
    void chooseKey();
    void shifter();
    void add();
    void edit();
    void editThresholds();
    void remove();
    void save();
    void cancel();

private:
    LRESULT send(int control, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) const
    {
        return SendDlgItemMessageW(window_, control, message, wParam, lParam);
    }

    const DeviceInfo* selectedDevice() const;
    void setStatus(const wchar_t* text) const;
    void startSampling(CaptureStage stage, const wchar_t* text);
    void finishCapture();
    void refreshBindings(LRESULT selection = LB_ERR);
    void renderPreview(std::wstring status, std::optional<int> travel);

    DirectInput& input_;
    const std::filesystem::path& root_;
    const ProfilePreset& preset_;
    std::filesystem::path path_;
    std::optional<ProfileFileRevision> revision_;
    HWND window_{};
    std::vector<DeviceInfo> devices_;
    AccessoryProfile bindings_;
    std::vector<AccessoryBinding> captured_;
    std::optional<AccessoryBinding> previewBinding_;
    std::optional<AccessoryReader> preview_;
    std::wstring previewStatus_;
    std::optional<int> previewTravel_;
    std::optional<DeviceSample> previous_;
    std::optional<AxisCapture> rest_;
    CalibrationCapture measurements_;
    CaptureStage stage_{CaptureStage::Idle};
    unsigned int key_{};
    ULONGLONG deadline_{};
    bool modified_{};
    bool previewPolling_{};
    bool previewAxis_{};
};

bool AccessoryDialog::initialise(HWND window)
{
    window_ = window;
    revision_ = profileFileRevision(path_);
    std::error_code fileError;
    if (std::filesystem::exists(path_, fileError) || fileError) {
        std::string error;
        const auto saved = loadAccessoryProfile(path_, error);
        if (!saved) {
            const auto message = tr(Text::DlgAccessoriesReadError) + localizedError(error);
            MessageBoxW(window_, message.c_str(), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONERROR);
            EndDialog(window_, IDCANCEL);
            return false;
        }
        bindings_ = *saved;
    }
    send(IDC_ACCESSORY_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(Text::DlgDigital)));
    send(IDC_ACCESSORY_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(Text::DlgAnalog)));
    send(IDC_ACCESSORY_TYPE, CB_SETCURSEL, 0);
    refreshDevices();
    SetFocus(GetDlgItem(window_, selectedDevice() ? IDC_ACCESSORY_DEVICE : IDC_ACCESSORY_REFRESH));
    return false;
}

const DeviceInfo* AccessoryDialog::selectedDevice() const
{
    const auto selected = send(IDC_ACCESSORY_DEVICE, CB_GETCURSEL);
    return selected >= 0 && static_cast<std::size_t>(selected) < devices_.size()
        ? &devices_[static_cast<std::size_t>(selected)] : nullptr;
}

void AccessoryDialog::setStatus(const wchar_t* text) const
{
    SetDlgItemTextW(window_, IDC_ACCESSORY_STATUS, text);
}

void AccessoryDialog::refreshDevices()
{
    const auto* current = selectedDevice();
    const auto currentId = current ? current->id : std::string{};
    devices_ = input_.devices();
    send(IDC_ACCESSORY_DEVICE, CB_RESETCONTENT);
    WPARAM selected = 0;
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        const auto& item = devices_[i];
        auto label = wide(item.name.empty() ? item.id : item.name);
        if (std::ranges::count(devices_, item.name, &DeviceInfo::name) > 1 && item.id.size() == 38)
            label += L" [" + wide(item.id.substr(29, 8)) + L"]";
        send(IDC_ACCESSORY_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (item.id == currentId)
            selected = i;
    }
    send(IDC_ACCESSORY_DEVICE, CB_SETCURSEL, selected);
    clearCapture();
    refreshBindings();
}

void AccessoryDialog::clearCapture()
{
    KillTimer(window_, captureTimer);
    stage_ = CaptureStage::Idle;
    captured_.clear();
    previous_.reset();
    rest_.reset();
    send(IDC_ACCESSORY_INPUT, CB_RESETCONTENT);
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, tr(Text::DlgAccessoriesCapture));
    if (const auto* device = selectedDevice()) {
        const auto status = tr(Text::DlgAccessoriesAxesCount) + std::to_wstring(std::ranges::count(device->axes, true))
            + L" · " + tr(Text::DlgAccessoriesButtonsCount) + std::to_wstring(device->buttons.count())
            + L" · " + tr(Text::DlgAccessoriesDpadsCount) + std::to_wstring(std::ranges::count(device->povs, true))
            + L"\r\n" + tr(Text::DlgAccessoriesAxisHint);
        setStatus(status.c_str());
    } else {
        setStatus(tr(Text::DlgAccessoriesConnectDevice));
    }
    refreshActions();
}

void AccessoryDialog::refreshActions()
{
    const auto row = send(IDC_ACCESSORY_LIST, LB_GETCURSEL);
    const bool selected = row >= 0 && static_cast<std::size_t>(row) < bindings_.size();
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_CAPTURE), selectedDevice() != nullptr);
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_INPUT), !captured_.empty());
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_KEY), stage_ == CaptureStage::Idle);
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_ADD),
        send(IDC_ACCESSORY_INPUT, CB_GETCURSEL) >= 0 && isAccessoryKey(key_));
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_EDIT), stage_ == CaptureStage::Idle && selected);
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_THRESHOLDS),
        stage_ == CaptureStage::Idle && selected && bindings_[static_cast<std::size_t>(row)].kind == AccessoryKind::Axis);
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_REMOVE), selected);
    EnableWindow(GetDlgItem(window_, IDOK), modified_);
    selectPreview();
}

void AccessoryDialog::selectCapturedInput()
{
    send(IDC_ACCESSORY_LIST, LB_SETCURSEL, static_cast<WPARAM>(-1));
    refreshActions();
}

void AccessoryDialog::selectPreview()
{
    std::optional<AccessoryBinding> binding;
    if (stage_ == CaptureStage::Idle) {
        const auto row = send(IDC_ACCESSORY_LIST, LB_GETCURSEL);
        const auto capture = send(IDC_ACCESSORY_INPUT, CB_GETCURSEL);
        if (row >= 0 && static_cast<std::size_t>(row) < bindings_.size()) {
            binding = bindings_[static_cast<std::size_t>(row)];
        } else if (capture >= 0 && static_cast<std::size_t>(capture) < captured_.size()) {
            binding = captured_[static_cast<std::size_t>(capture)];
            binding->key = key_;
        }
    }
    if (binding != previewBinding_) {
        previewBinding_ = std::move(binding);
        preview_.reset();
        if (previewBinding_) {
            auto mapping = *previewBinding_;
            if (!isAccessoryKey(mapping.key))
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
            const auto thresholds = previewBinding_->thresholds;
            const auto limits = tr(Text::DlgAccessoryPreviewPress) + percent(static_cast<int>(thresholds.press))
                + L"  ·  " + tr(Text::DlgAccessoryPreviewRelease) + percent(static_cast<int>(thresholds.release));
            SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_LIMITS, limits.c_str());
            SetDlgItemTextW(window_, IDC_ACCESSORY_PREVIEW_LEVEL,
                                (tr(Text::DlgAccessoryPreviewTravel) + std::wstring(L"—")).c_str());
            InvalidateRect(GetDlgItem(window_, IDC_ACCESSORY_PREVIEW_METER), nullptr, FALSE);
        }
        renderPreview(tr(Text::DlgAccessoryPreviewPaused), std::nullopt);
    }
    setPreviewFocus(GetForegroundWindow() == window_);
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
        KillTimer(window_, previewTimer);
        previewPolling_ = false;
        if (preview_)
            preview_->reset();
    }
    renderPreview(tr(stage_ != CaptureStage::Idle || previewBinding_
        ? Text::DlgAccessoryPreviewPaused : Text::DlgAccessoryPreviewSelect), std::nullopt);
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
    input_.beginPoll();
    preview_->poll(input_, observations);
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
        case AccessoryStatus::temporarilyUnavailable: return Text::DlgAccessoryPreviewPending;
        case AccessoryStatus::waitingForNeutral: return Text::DlgAccessoryPreviewNeutral;
        case AccessoryStatus::armed:
            return !isAccessoryKey(previewBinding_->key) ? Text::DlgAccessoryPreviewUnassigned
                : observation.active ? Text::DlgAccessoryPreviewPressed : Text::DlgAccessoryPreviewReleased;
        }
        return Text::DlgAccessoryPreviewInput;
    }();
    auto message = std::wstring(tr(status));
    if (isAccessoryKey(previewBinding_->key) && observation.status == AccessoryStatus::armed)
        message = accessoryKeyLabel(previewBinding_->key) + message;
    renderPreview(std::move(message), travel);
}

void AccessoryDialog::renderPreview(std::wstring status, std::optional<int> travel)
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
    const auto line = [&](unsigned int value, COLORREF marker) {
        const auto position = std::clamp(x(static_cast<int>(value)), rect.left, rect.right - 1);
        ui::fill(item.hDC, {position, rect.top, position + 1, rect.bottom}, marker);
    };
    line(previewBinding_->thresholds.release, color(ui::ink, COLOR_WINDOWTEXT));
    line(previewBinding_->thresholds.press, color(ui::yellow, COLOR_WINDOWTEXT));
}

void AccessoryDialog::chooseKey()
{
    if (stage_ != CaptureStage::Idle)
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

void AccessoryDialog::shifter()
{
    clearCapture();
    const auto* device = selectedDevice();
    if (editShifter(window_, input_, bindings_, device ? std::string_view(device->id) : std::string_view{})) {
        modified_ = true;
        refreshBindings();
        setStatus(tr(Text::DlgAccessoriesShifterConfigured));
    }
}

void AccessoryDialog::startSampling(CaptureStage stage, const wchar_t* text)
{
    if (!SetTimer(window_, captureTimer, captureIntervalMs, nullptr)) {
        clearCapture();
        setStatus(tr(Text::DlgAccessoriesCaptureFailed));
        return;
    }
    measurements_ = {};
    stage_ = stage;
    deadline_ = GetTickCount64() + captureTimeoutMs;
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, tr(Text::DlgAccessoriesCancelCapture));
    setStatus(text);
    refreshActions();
}

void AccessoryDialog::capture()
{
    const auto* device = selectedDevice();
    if (!device)
        return;
    if (stage_ == CaptureStage::AwaitingTravel) {
        startSampling(CaptureStage::AxisTravel, tr(Text::DlgAccessoriesHoldAtLimit));
        return;
    }
    const auto wasIdle = stage_ == CaptureStage::Idle;
    clearCapture();
    if (!wasIdle)
        return;
    if (send(IDC_ACCESSORY_TYPE, CB_GETCURSEL) == 1) {
        if (std::ranges::none_of(device->axes, [](bool present) { return present; })) {
            setStatus(tr(Text::DlgAccessoriesNoAbsoluteAxes));
            return;
        }
        startSampling(CaptureStage::AxisRest, tr(Text::DlgAccessoriesHoldAtRest));
    } else {
        input_.beginPoll();
        previous_ = input_.sample(device->id);
        if (!previous_) {
            setStatus(tr(Text::DlgAccessoriesInputUnavailable));
            return;
        }
        startSampling(CaptureStage::Digital, tr(Text::DlgAccessoriesPressInput));
    }
}

void AccessoryDialog::sample()
{
    if (!IsWindowEnabled(window_) || stage_ == CaptureStage::Idle || stage_ == CaptureStage::AwaitingTravel)
        return;
    if (GetForegroundWindow() != window_) {
        clearCapture();
        setStatus(tr(Text::DlgAccessoriesCaptureInterrupted));
        return;
    }
    if (GetTickCount64() >= deadline_) {
        clearCapture();
        setStatus(tr(Text::DlgAccessoriesCaptureExpired));
        return;
    }
    const auto* device = selectedDevice();
    input_.beginPoll();
    const auto state = device ? input_.sample(device->id) : std::nullopt;
    if (!state || GetTickCount64() >= deadline_) {
        clearCapture();
        setStatus(tr(state ? Text::DlgAccessoriesCaptureExpired : Text::DlgAccessoriesInputUnavailable));
        return;
    }
    if (stage_ == CaptureStage::Digital) {
        const auto pressed = state->buttons & ~previous_->buttons;
        for (std::size_t i = 0; i < buttonCount; ++i) {
            if (pressed[i])
                captured_.push_back({device->id, AccessoryKind::Button, i, 0, 1, 0});
        }
        for (std::size_t i = 0; i < povCount; ++i) {
            const auto direction = quantizePov(state->povs[i]);
            if (direction != povCentered && direction != quantizePov(previous_->povs[i]))
                captured_.push_back({device->id, AccessoryKind::Pov, i, povCentered, direction, 0});
        }
        previous_ = state;
        if (!captured_.empty())
            finishCapture();
        return;
    }
    if (!measurements_.add(*state)) {
        measurements_ = {};
        return;
    }
    const auto measured = measurements_.result();
    if (!measured)
        return;
    if (stage_ == CaptureStage::AxisRest) {
        KillTimer(window_, captureTimer);
        rest_ = measured;
        stage_ = CaptureStage::AwaitingTravel;
        SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, tr(Text::DlgAccessoriesMeasureLimit));
        setStatus(tr(Text::DlgAccessoriesMeasureLimitHint));
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
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, tr(Text::DlgAccessoriesCapture));
    for (const auto& binding : captured_) {
        const auto label = accessoryInputLabel(binding);
        send(IDC_ACCESSORY_INPUT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    send(IDC_ACCESSORY_INPUT, CB_SETCURSEL, 0);
    setStatus(captured_.empty() ? tr(Text::DlgAccessoriesAxisDidNotMove)
        : captured_.size() == 1 ? tr(Text::DlgAccessoriesChooseKeyAndAdd)
        : tr(Text::DlgAccessoriesMultipleInputs));
    selectCapturedInput();
}

void AccessoryDialog::refreshBindings(LRESULT selection)
{
    const auto top = send(IDC_ACCESSORY_LIST, LB_GETTOPINDEX);
    send(IDC_ACCESSORY_LIST, LB_RESETCONTENT);
    for (const auto& binding : bindings_) {
        const auto device = std::ranges::find(devices_, binding.deviceId, &DeviceInfo::id);
        auto label = accessoryKeyLabel(binding.key) + L"  ←  " + accessoryBindingInputLabel(binding);
        if (binding.kind == AccessoryKind::Axis) {
            label += L" · " + std::to_wstring(binding.thresholds.press) + L"% / "
                + std::to_wstring(binding.thresholds.release) + L"%";
        }
        label += L"  ·  " + wide(device == devices_.end() || device->name.empty() ? binding.deviceId : device->name);
        send(IDC_ACCESSORY_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (selection >= 0) {
        send(IDC_ACCESSORY_LIST, LB_SETCURSEL, static_cast<WPARAM>(selection));
        if (top >= 0)
            send(IDC_ACCESSORY_LIST, LB_SETTOPINDEX, static_cast<WPARAM>(top));
    }
    refreshActions();
}

void AccessoryDialog::add()
{
    const auto input = send(IDC_ACCESSORY_INPUT, CB_GETCURSEL);
    if (input < 0 || static_cast<std::size_t>(input) >= captured_.size() || !isAccessoryKey(key_))
        return;
    if (std::ranges::find(bindings_, key_, &AccessoryBinding::key) != bindings_.end()) {
        setStatus(tr(Text::DlgAccessoriesKeyAlreadyAssigned));
        return;
    }
    if (bindings_.size() >= maxAccessoryBindings) {
        setStatus(tr(Text::DlgAccessoriesBindingLimit));
        return;
    }
    auto binding = captured_[static_cast<std::size_t>(input)];
    binding.key = key_;
    bindings_.push_back(std::move(binding));
    modified_ = true;
    clearCapture();
    refreshBindings(static_cast<LRESULT>(bindings_.size() - 1));
    setStatus(tr(Text::DlgAccessoriesBindingAdded));
}

void AccessoryDialog::edit()
{
    if (stage_ != CaptureStage::Idle)
        return;
    const auto selection = send(IDC_ACCESSORY_LIST, LB_GETCURSEL);
    if (selection < 0 || static_cast<std::size_t>(selection) >= bindings_.size())
        return;
    switch (chooseBindingKey(window_, bindings_, static_cast<std::size_t>(selection))) {
    case BindingKeyEdit::unchanged:
        break;
    case BindingKeyEdit::occupied:
        setStatus(tr(Text::DlgAccessoriesKeyAlreadyAssigned));
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
    const auto selection = send(IDC_ACCESSORY_LIST, LB_GETCURSEL);
    if (selection < 0 || static_cast<std::size_t>(selection) >= bindings_.size())
        return;
    auto& binding = bindings_[static_cast<std::size_t>(selection)];
    if (binding.kind != AccessoryKind::Axis)
        return;
    auto values = binding.thresholds;
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ACCESSORY_THRESHOLDS),
        window_, thresholdsProc, reinterpret_cast<LPARAM>(&values));
    if (result == -1) {
        MessageBoxW(window_, tr(Text::DlgThresholdsOpenError), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONERROR);
    } else if (result == IDOK && values != binding.thresholds) {
        binding.thresholds = values;
        modified_ = true;
        refreshBindings(selection);
        setStatus(tr(Text::DlgAccessoriesThresholdsEdited));
    }
}

void AccessoryDialog::remove()
{
    const auto index = send(IDC_ACCESSORY_LIST, LB_GETCURSEL);
    if (index < 0 || static_cast<std::size_t>(index) >= bindings_.size())
        return;
    bindings_.erase(bindings_.begin() + index);
    modified_ = true;
    refreshBindings();
    setStatus(tr(Text::DlgAccessoriesBindingRemoved));
}

void AccessoryDialog::save()
{
    if (!modified_)
        return;
    if (isTemporaryProfileDirectory(path_.parent_path())) {
        MessageBoxW(window_, tr(Text::DlgAccessoriesExtractBeforeSaving), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONWARNING);
        return;
    }
    std::string error;
    bool createFailed = false;
    const std::array checks{ProfileFileCheck{accessoryFilename, revision_}};
    const auto result = savePresetSettings(root_, preset_, checks, [&] {
        std::error_code directoryError;
        std::filesystem::create_directories(path_.parent_path(), directoryError);
        createFailed = static_cast<bool>(directoryError);
        return !createFailed && saveAccessoryProfile(path_, bindings_, error);
    });
    if (result == PresetSaveResult::changed) {
        MessageBoxW(window_, tr(Text::DlgAccessoriesChanged), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONWARNING);
        return;
    }
    if (result != PresetSaveResult::saved) {
        auto message = std::wstring(tr(createFailed ? Text::DlgAccessoriesCreateProfileFailed : Text::DlgAccessoriesSaveError));
        if (!error.empty())
            message += localizedError(error);
        MessageBoxW(window_, message.c_str(), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONERROR);
        return;
    }
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
        SetWindowTextW(window, tr(Text::DlgAccessoriesTitle));
        SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(Text::DlgAccessoriesIntro));
        SetDlgItemTextW(window, IDC_ACCESSORY_SHIFTER, tr(Text::DlgAccessoriesShifter));
        SetDlgItemTextW(window, IDC_MODAL_DEVICE_LABEL, tr(Text::DlgDevice));
        SetDlgItemTextW(window, IDC_ACCESSORY_REFRESH, tr(Text::DlgRefresh));
        SetDlgItemTextW(window, IDC_ACCESSORY_TYPE_LABEL, tr(Text::DlgAccessoriesInputType));
        SetDlgItemTextW(window, IDC_ACCESSORY_INPUT_LABEL, tr(Text::DlgAccessoriesCapturedInput));
        SetDlgItemTextW(window, IDC_MODAL_KEY_LABEL, tr(Text::DlgKey));
        SetDlgItemTextW(window, IDC_ACCESSORY_KEY, tr(Text::DlgChooseKey));
        SetDlgItemTextW(window, IDC_ACCESSORY_ADD, tr(Text::DlgAccessoriesAdd));
        SetDlgItemTextW(window, IDC_MODAL_LIST_HEADING, tr(Text::DlgAccessoriesSavedList));
        SetDlgItemTextW(window, IDC_ACCESSORY_EDIT, tr(Text::DlgBindingEdit));
        SetDlgItemTextW(window, IDC_ACCESSORY_THRESHOLDS, tr(Text::DlgAccessoriesThresholds));
        SetDlgItemTextW(window, IDC_ACCESSORY_PREVIEW_HEADING, tr(Text::DlgAccessoryPreview));
        SetDlgItemTextW(window, IDC_ACCESSORY_REMOVE, tr(Text::DlgAccessoriesRemove));
        SetDlgItemTextW(window, IDOK, tr(Text::DlgSave));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, tr(Text::DlgAccessoriesHeading), IDC_ACCESSORY_STATUS, IDC_ACCESSORY_LIST,
                            {IDC_ACCESSORY_CAPTURE, IDC_ACCESSORY_ADD, IDOK});
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
    }
    if (message == WM_ACTIVATE || message == WM_ENABLE) {
        const bool focused = message == WM_ACTIVATE ? LOWORD(wParam) != WA_INACTIVE : wParam != 0;
        dialog->setPreviewFocus(focused);
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
    case IDC_ACCESSORY_SHIFTER:
        dialog->shifter();
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

bool editAccessories(HWND owner, DirectInput& input, const std::filesystem::path& root, const ProfilePreset& preset)
{
    AccessoryDialog dialog(input, root, preset);
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ACCESSORIES), owner,
        dialogProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgAccessoriesOpenError), tr(Text::DlgAccessoriesTitle), MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
