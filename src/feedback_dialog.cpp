#include "chrome_axis/feedback_dialog.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/feedback.hpp"
#include "dialog_theme.hpp"

namespace chrome_axis {
namespace {

constexpr wchar_t title[] = L"Retour de force · Chrome Axis";

enum class Compatibility { noCalibration, disconnected, unsupported, supported };

Compatibility compatibility(const DeviceInfo* device, std::size_t axis)
{
    if (axis >= axisCount)
        return Compatibility::noCalibration;
    if (!device)
        return Compatibility::disconnected;
    return device->forceFeedbackAxes[axis] ? Compatibility::supported : Compatibility::unsupported;
}

const wchar_t* compatibilityText(Compatibility state)
{
    switch (state) {
    case Compatibility::noCalibration:
        return L"Enregistrez d’abord la calibration de l’axe de direction.";
    case Compatibility::disconnected:
        return L"Le volant de ce profil est déconnecté. Rebranchez-le, puis actualisez les périphériques.";
    case Compatibility::unsupported:
        return L"Le pilote DirectInput n’annonce pas de retour de force sur l’axe de direction calibré.";
    case Compatibility::supported:
        return L"Axe de direction compatible. Le retour de force s’applique en jeu avec l’USB activé.";
    }
    return L"";
}

void addChoice(HWND window, int control, const wchar_t* text)
{
    SendDlgItemMessageW(window, control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

void addToggle(HWND window, int control, const wchar_t* disabled, const wchar_t* enabled, bool selection)
{
    addChoice(window, control, disabled);
    addChoice(window, control, enabled);
    SendDlgItemMessageW(window, control, CB_SETCURSEL, selection ? 1 : 0, 0);
}

struct FeedbackDialog {
    std::filesystem::path file;
    Compatibility state;

    void initialise(HWND window) const;
    void updateControls(HWND window) const;
    void save(HWND window) const;
};

void FeedbackDialog::initialise(HWND window) const
{
    std::string error;
    const auto saved = loadFeedbackSettings(file, error);
    const auto settings = saved.value_or(FeedbackSettings{});
    const bool supported = state == Compatibility::supported;

    SetDlgItemTextW(window, IDC_FEEDBACK_STATUS, compatibilityText(state));
    if (!saved) {
        SetDlgItemTextW(window, IDC_MODAL_INTRO,
            L"Les réglages enregistrés sont illisibles. Enregistrez pour les remplacer.");
    } else if (settings.enabled && !supported) {
        SetDlgItemTextW(window, IDC_MODAL_INTRO,
            L"Enregistrez pour désactiver le retour de force de ce profil.");
    }

    if (supported) {
        addToggle(window, IDC_FEEDBACK_ENABLE, L"Désactivé", L"Activé", settings.enabled);
    } else {
        addChoice(window, IDC_FEEDBACK_ENABLE, L"Désactivé");
        SendDlgItemMessageW(window, IDC_FEEDBACK_ENABLE, CB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(window, IDC_FEEDBACK_ENABLE), FALSE);
    }
    for (int value = 1; value <= maxFeedbackPercent; ++value) {
        const auto label = std::to_wstring(value) + L" %";
        addChoice(window, IDC_FEEDBACK_STRENGTH, label.c_str());
    }
    SendDlgItemMessageW(window, IDC_FEEDBACK_STRENGTH, CB_SETCURSEL, static_cast<WPARAM>(settings.strength - 1), 0);
    addToggle(window, IDC_FEEDBACK_IMPACTS, L"Désactivés", L"Activés", settings.impacts);
    addToggle(window, IDC_FEEDBACK_DIRECTION, L"Normal", L"Inversé", settings.invert);
    updateControls(window);
}

void FeedbackDialog::updateControls(HWND window) const
{
    const bool enabled = state == Compatibility::supported
        && SendDlgItemMessageW(window, IDC_FEEDBACK_ENABLE, CB_GETCURSEL, 0, 0) == 1;
    for (const int id : {IDC_FEEDBACK_STRENGTH, IDC_FEEDBACK_IMPACTS, IDC_FEEDBACK_DIRECTION})
        EnableWindow(GetDlgItem(window, id), enabled);
}

void FeedbackDialog::save(HWND window) const
{
    if (isTemporaryProfileDirectory(file.parent_path())) {
        MessageBoxW(window, L"Extrayez le ZIP ou ouvrez Chrome Axis depuis le dossier du jeu avant "
            L"d’enregistrer le retour de force.", title, MB_OK | MB_ICONWARNING);
        return;
    }
    const auto selected = [&](int id) { return SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0); };
    const auto strength = selected(IDC_FEEDBACK_STRENGTH);
    if (strength < 0 || strength >= maxFeedbackPercent)
        return;

    const FeedbackSettings settings{
        state == Compatibility::supported && selected(IDC_FEEDBACK_ENABLE) == 1,
        static_cast<int>(strength) + 1,
        selected(IDC_FEEDBACK_IMPACTS) == 1,
        selected(IDC_FEEDBACK_DIRECTION) == 1,
    };
    std::string error;
    if (!saveFeedbackSettings(file, settings, error)) {
        MessageBoxW(window, L"Impossible d’enregistrer le retour de force. Vérifiez que le dossier Profil est accessible en écriture.",
                    title, MB_OK | MB_ICONERROR);
        return;
    }
    EndDialog(window, IDOK);
}

INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        DialogTheme::attach(window, L"Retour de force", IDC_FEEDBACK_STATUS, 0, {IDOK});
        reinterpret_cast<const FeedbackDialog*>(lParam)->initialise(window);
        return TRUE;
    }
    const auto* dialog = reinterpret_cast<const FeedbackDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog || message != WM_COMMAND)
        return FALSE;
    switch (LOWORD(wParam)) {
    case IDC_FEEDBACK_ENABLE:
        if (HIWORD(wParam) == CBN_SELCHANGE)
            dialog->updateControls(window);
        return TRUE;
    case IDOK:
        dialog->save(window);
        return TRUE;
    case IDCANCEL:
        EndDialog(window, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

bool editForceFeedback(HWND owner, const std::filesystem::path& directory,
                       const DeviceInfo* steeringDevice, std::size_t steeringAxis)
{
    if (!directory.is_absolute())
        return false;
    const FeedbackDialog dialog{directory / feedbackFilename, compatibility(steeringDevice, steeringAxis)};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FORCE_FEEDBACK), owner,
                                        dialogProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, L"Impossible d’ouvrir les réglages du retour de force.", title, MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
