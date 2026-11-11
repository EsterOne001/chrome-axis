#include "chrome_axis/accessories_dialog.hpp"
#include "dialog_theme.hpp"

#include "chrome_axis/accessories.hpp"
#include "chrome_axis/calibration.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/keyboard_dialog.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace chrome_axis {

std::wstring accessoryKeyLabel(unsigned int key)
{
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return L"Pavé " + std::to_wstring(key - VK_NUMPAD0);
    if (key >= VK_F1 && key <= VK_F12)
        return L"F" + std::to_wstring(key - VK_F1 + 1);
    switch (key) {
    case VK_SPACE: return L"Espace";
    case VK_UP: return L"Flèche haut";
    case VK_DOWN: return L"Flèche bas";
    case VK_LEFT: return L"Flèche gauche";
    case VK_RIGHT: return L"Flèche droite";
    case VK_ADD: return L"Pavé +";
    case VK_SUBTRACT: return L"Pavé −";
    default: return std::wstring(1, static_cast<wchar_t>(key));
    }
}

namespace {

constexpr wchar_t title[] = L"Accessoires · Chrome Axis";
constexpr UINT_PTR captureTimer = 1;
constexpr UINT captureIntervalMs = 30;
constexpr ULONGLONG captureTimeoutMs = 12000;

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

std::wstring inputLabel(const AccessoryBinding& binding)
{
    constexpr std::array directions{L"Nord", L"Nord-est", L"Est", L"Sud-est", L"Sud", L"Sud-ouest", L"Ouest", L"Nord-ouest"};
    switch (binding.kind) {
    case AccessoryKind::Button:
        return L"Bouton " + std::to_wstring(binding.index + 1);
    case AccessoryKind::Pov:
        return L"Croix " + std::to_wstring(binding.index + 1) + L" · " + directions[binding.engaged / 4500];
    case AccessoryKind::Axis:
        return L"Axe " + wide(axisName(binding.index));
    }
    return {};
}

long povDirection(std::uint16_t value)
{
    return value == povCentered ? povCentered : ((value + 2250) / 4500 % 8) * 4500;
}

class AccessoryDialog {
public:
    AccessoryDialog(DirectInput& input, std::filesystem::path path)
        : input_(input), path_(std::move(path)) {}

    bool initialise(HWND window);
    void capture();
    void sample();
    void refreshDevices();
    void clearCapture();
    void refreshActions();
    void chooseKey();
    void shifter();
    void add();
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
    void refreshBindings();

    DirectInput& input_;
    std::filesystem::path path_;
    HWND window_{};
    std::vector<DeviceInfo> devices_;
    AccessoryProfile bindings_;
    std::vector<AccessoryBinding> captured_;
    std::optional<DeviceSample> previous_;
    std::optional<AxisCapture> rest_;
    CalibrationCapture measurements_;
    CaptureStage stage_{CaptureStage::Idle};
    unsigned int key_{};
    ULONGLONG deadline_{};
    bool modified_{};
};

bool AccessoryDialog::initialise(HWND window)
{
    window_ = window;
    std::error_code fileError;
    if (std::filesystem::exists(path_, fileError) || fileError) {
        std::string error;
        const auto saved = loadAccessoryProfile(path_, error);
        if (!saved) {
            const auto message = L"Impossible de lire les associations enregistrées. Corrigez ou déplacez "
                L"le fichier accessories.ini du dossier Profil, puis rouvrez ce panneau.\n" + wide(error);
            MessageBoxW(window_, message.c_str(), title, MB_OK | MB_ICONERROR);
            EndDialog(window_, IDCANCEL);
            return false;
        }
        bindings_ = *saved;
    }
    send(IDC_ACCESSORY_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bouton ou croix"));
    send(IDC_ACCESSORY_TYPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Axe analogique"));
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
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, L"&Capturer");
    if (const auto* device = selectedDevice()) {
        const auto status = std::to_wstring(std::ranges::count(device->axes, true)) + L" axes · "
            + std::to_wstring(device->buttons.count()) + L" boutons · "
            + std::to_wstring(std::ranges::count(device->povs, true)) + L" croix.\r\n"
            + L"Pour mesurer un axe, placez-le d’abord au repos.";
        setStatus(status.c_str());
    } else {
        setStatus(L"Branchez un périphérique DirectInput, puis cliquez sur Actualiser.");
    }
    refreshActions();
}

void AccessoryDialog::refreshActions()
{
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_CAPTURE), selectedDevice() != nullptr);
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_INPUT), !captured_.empty());
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_KEY), stage_ == CaptureStage::Idle);
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_ADD),
        send(IDC_ACCESSORY_INPUT, CB_GETCURSEL) >= 0 && isAccessoryKey(key_));
    EnableWindow(GetDlgItem(window_, IDC_ACCESSORY_REMOVE), send(IDC_ACCESSORY_LIST, LB_GETCURSEL) >= 0);
    EnableWindow(GetDlgItem(window_, IDOK), modified_);
}

void AccessoryDialog::chooseKey()
{
    if (stage_ != CaptureStage::Idle)
        return;
    if (const auto key = chooseAccessoryKey(window_, key_)) {
        key_ = *key;
        const auto label = accessoryKeyLabel(key_) + L"…";
        SetDlgItemTextW(window_, IDC_ACCESSORY_KEY, label.c_str());
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
        setStatus(L"Boîte ou palettes configurées. Cliquez sur Enregistrer pour appliquer les affectations.");
    }
}

void AccessoryDialog::startSampling(CaptureStage stage, const wchar_t* text)
{
    if (!SetTimer(window_, captureTimer, captureIntervalMs, nullptr)) {
        clearCapture();
        setStatus(L"Impossible de démarrer la capture.");
        return;
    }
    measurements_ = {};
    stage_ = stage;
    deadline_ = GetTickCount64() + captureTimeoutMs;
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, L"Annuler la &capture");
    setStatus(text);
    refreshActions();
}

void AccessoryDialog::capture()
{
    const auto* device = selectedDevice();
    if (!device)
        return;
    if (stage_ == CaptureStage::AwaitingTravel) {
        startSampling(CaptureStage::AxisTravel, L"Maintenez l’axe en butée pendant la mesure.");
        return;
    }
    const auto wasIdle = stage_ == CaptureStage::Idle;
    clearCapture();
    if (!wasIdle)
        return;
    if (send(IDC_ACCESSORY_TYPE, CB_GETCURSEL) == 1) {
        if (std::ranges::none_of(device->axes, [](bool present) { return present; })) {
            setStatus(L"Ce périphérique ne signale aucun axe analogique absolu.");
            return;
        }
        startSampling(CaptureStage::AxisRest, L"Laissez l’axe au repos pendant la mesure.");
    } else {
        previous_ = input_.sample(device->id);
        if (!previous_) {
            setStatus(L"Entrées indisponibles. Vérifiez le branchement, puis réessayez.");
            return;
        }
        startSampling(CaptureStage::Digital, L"Appuyez sur un bouton ou actionnez la croix du périphérique.");
    }
}

void AccessoryDialog::sample()
{
    if (!IsWindowEnabled(window_) || stage_ == CaptureStage::Idle || stage_ == CaptureStage::AwaitingTravel)
        return;
    if (GetForegroundWindow() != window_) {
        clearCapture();
        setStatus(L"Capture interrompue. Revenez dans cette fenêtre et relancez la mesure.");
        return;
    }
    const auto* device = selectedDevice();
    const auto state = device ? input_.sample(device->id) : std::nullopt;
    if (!state || GetTickCount64() >= deadline_) {
        clearCapture();
        setStatus(state ? L"Capture expirée. Relancez la mesure pour réessayer."
                        : L"Entrées indisponibles. Vérifiez le branchement, puis réessayez.");
        return;
    }
    if (stage_ == CaptureStage::Digital) {
        const auto pressed = state->buttons & ~previous_->buttons;
        for (std::size_t i = 0; i < buttonCount; ++i) {
            if (pressed[i])
                captured_.push_back({device->id, AccessoryKind::Button, i, 0, 1, 0});
        }
        for (std::size_t i = 0; i < povCount; ++i) {
            const auto direction = povDirection(state->povs[i]);
            if (direction != povCentered && direction != povDirection(previous_->povs[i]))
                captured_.push_back({device->id, AccessoryKind::Pov, i, povCentered, direction, 0});
        }
        previous_ = state;
        if (!captured_.empty())
            finishCapture();
        return;
    }
    measurements_.add(*state);
    const auto measured = measurements_.result();
    if (!measured)
        return;
    if (stage_ == CaptureStage::AxisRest) {
        KillTimer(window_, captureTimer);
        rest_ = measured;
        stage_ = CaptureStage::AwaitingTravel;
        SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, L"Mesurer la &butée");
        setStatus(L"Actionnez l’axe à fond et maintenez-le, puis cliquez sur Mesurer la butée.");
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
    SetDlgItemTextW(window_, IDC_ACCESSORY_CAPTURE, L"&Capturer");
    for (const auto& binding : captured_) {
        const auto label = inputLabel(binding);
        send(IDC_ACCESSORY_INPUT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    send(IDC_ACCESSORY_INPUT, CB_SETCURSEL, 0);
    setStatus(captured_.empty() ? L"Aucun axe suffisamment déplacé. Recommencez en atteignant la butée."
        : captured_.size() == 1 ? L"Choisissez une touche et ajoutez l’association."
        : L"Plusieurs entrées ont réagi. Choisissez l’entrée à associer et sa touche.");
    refreshActions();
}

void AccessoryDialog::refreshBindings()
{
    send(IDC_ACCESSORY_LIST, LB_RESETCONTENT);
    for (const auto& binding : bindings_) {
        const auto device = std::ranges::find(devices_, binding.deviceId, &DeviceInfo::id);
        const auto label = accessoryKeyLabel(binding.key) + L"  ←  " + inputLabel(binding) + L"  ·  "
            + wide(device == devices_.end() || device->name.empty() ? binding.deviceId : device->name);
        send(IDC_ACCESSORY_LIST, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    refreshActions();
}

void AccessoryDialog::add()
{
    const auto input = send(IDC_ACCESSORY_INPUT, CB_GETCURSEL);
    if (input < 0 || static_cast<std::size_t>(input) >= captured_.size() || !isAccessoryKey(key_))
        return;
    if (std::ranges::find(bindings_, key_, &AccessoryBinding::key) != bindings_.end()) {
        setStatus(L"Cette touche est déjà associée. Retirez l’association existante pour la réutiliser.");
        return;
    }
    if (bindings_.size() >= maxAccessoryBindings) {
        setStatus(L"Limite d’associations atteinte. Retirez une association pour libérer une place.");
        return;
    }
    auto binding = captured_[static_cast<std::size_t>(input)];
    binding.key = key_;
    bindings_.push_back(std::move(binding));
    modified_ = true;
    clearCapture();
    refreshBindings();
    setStatus(L"Association ajoutée. Enregistrez pour l’appliquer dans le jeu.");
}

void AccessoryDialog::remove()
{
    const auto index = send(IDC_ACCESSORY_LIST, LB_GETCURSEL);
    if (index < 0 || static_cast<std::size_t>(index) >= bindings_.size())
        return;
    bindings_.erase(bindings_.begin() + index);
    modified_ = true;
    refreshBindings();
    setStatus(L"Association retirée. Enregistrez pour l’appliquer dans le jeu.");
}

void AccessoryDialog::save()
{
    if (!modified_)
        return;
    if (isTemporaryProfileDirectory(path_.parent_path())) {
        MessageBoxW(window_, L"Extrayez le ZIP ou ouvrez Chrome Axis depuis le dossier du jeu avant "
            L"d’enregistrer les associations.", title, MB_OK | MB_ICONWARNING);
        return;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(path_.parent_path(), directoryError);
    if (directoryError) {
        MessageBoxW(window_, L"Impossible de créer le dossier Profil. Vérifiez les droits d’écriture "
            L"dans le dossier de Chrome Axis.", title, MB_OK | MB_ICONERROR);
        return;
    }
    std::string error;
    if (!saveAccessoryProfile(path_, bindings_, error)) {
        const auto message = L"Impossible d’enregistrer les associations.\n" + wide(error);
        MessageBoxW(window_, message.c_str(), title, MB_OK | MB_ICONERROR);
        return;
    }
    EndDialog(window_, IDOK);
}

void AccessoryDialog::cancel()
{
    if (!modified_ || MessageBoxW(window_, L"Fermer sans enregistrer les associations modifiées ?",
        title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        EndDialog(window_, IDCANCEL);
}

INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        auto& dialog = *reinterpret_cast<AccessoryDialog*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        DialogTheme::attach(window, L"Accessoires", IDC_ACCESSORY_STATUS, IDC_ACCESSORY_LIST,
                            {IDC_ACCESSORY_CAPTURE, IDC_ACCESSORY_ADD, IDOK});
        return dialog.initialise(window);
    }
    auto* dialog = reinterpret_cast<AccessoryDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog)
        return FALSE;
    if (message == WM_TIMER && wParam == captureTimer) {
        dialog->sample();
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
    case IDC_ACCESSORY_LIST:
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

bool editAccessories(HWND owner, DirectInput& input, const std::filesystem::path& profileDirectory)
{
    AccessoryDialog dialog(input, profileDirectory / accessoryFilename);
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ACCESSORIES), owner,
        dialogProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, L"Impossible d’ouvrir le panneau des accessoires.", title, MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
