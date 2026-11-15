#include "chrome_axis/feedback_dialog.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/feedback.hpp"
#include "app_text.hpp"
#include "dialog_theme.hpp"
#include "ui_theme.hpp"

#include <commctrl.h>
#include <algorithm>
#include <array>
constexpr int gainStep = 5;
namespace chrome_axis {
namespace {


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
        return tr(Text::DlgFeedbackNoCalibration);
    case Compatibility::disconnected:
        return tr(Text::DlgFeedbackDisconnected);
    case Compatibility::unsupported:
        return tr(Text::DlgFeedbackUnsupported);
    case Compatibility::supported:
        return tr(Text::DlgFeedbackSupported);
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

int snapGain(int percent)
{
    return std::clamp((percent + gainStep / 2) / gainStep * gainStep, gainStep, maxFeedbackGainPercent);
}

void paintGainSlider(const NMCUSTOMDRAW& draw)
{
    const auto slider = draw.hdr.hwndFrom;
    RECT bounds{}, track{}, thumb{};
    GetClientRect(slider, &bounds);
    SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&track));
    SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const LONG x = (thumb.left + thumb.right) / 2;
    const LONG y = (thumb.top + thumb.bottom) / 2;
    const UINT dpi = GetDpiForWindow(slider);
    const bool enabled = IsWindowEnabled(slider) != FALSE;
    const auto highlight = enabled ? ui::cyan : ui::edge;
    const LONG thickness = ui::px(2, dpi);
    const LONG width = ui::px(4, dpi);
    ui::fill(draw.hdc, bounds, ui::panel);
    ui::fill(draw.hdc, {track.left, y - thickness, track.right, y + thickness}, ui::edge);
    ui::fill(draw.hdc, {track.left, y - thickness, x, y + thickness}, highlight);
    ui::fill(draw.hdc, {x - width, thumb.top, x + width, thumb.bottom}, highlight);
    if (GetFocus() == slider && !(SendMessageW(slider, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        DrawFocusRect(draw.hdc, &bounds);
}

struct FeedbackDialog {
    const std::filesystem::path& root;
    const ProfilePreset& preset;
    const std::optional<ProfileFileRevision>& calibrationRevision;
    std::filesystem::path file;
    std::optional<ProfileFileRevision> revision;
    Compatibility state;
    int gain{};

    void initialise(HWND window);
    void updateControls(HWND window) const;
    void displayGain(HWND window) const;
    void selectGain(HWND window);
    void save(HWND window) const;
};

void FeedbackDialog::initialise(HWND window)
{
    std::string error;
    const auto saved = loadFeedbackSettings(file, error);
    const auto settings = saved.value_or(FeedbackSettings{});
    const bool supported = state == Compatibility::supported;

    SetDlgItemTextW(window, IDC_FEEDBACK_STATUS, compatibilityText(state));
    if (!saved) {
        SetDlgItemTextW(window, IDC_MODAL_INTRO,
            tr(Text::DlgFeedbackUnreadable));
    } else if (settings.enabled && !supported) {
        SetDlgItemTextW(window, IDC_MODAL_INTRO,
            tr(Text::DlgFeedbackDisableProfile));
    }

    if (supported) {
        addToggle(window, IDC_FEEDBACK_ENABLE, tr(Text::DlgFeedbackDisabled), tr(Text::DlgFeedbackEnabled), settings.enabled);
    } else {
        addChoice(window, IDC_FEEDBACK_ENABLE, tr(Text::DlgFeedbackDisabled));
        SendDlgItemMessageW(window, IDC_FEEDBACK_ENABLE, CB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(window, IDC_FEEDBACK_ENABLE), FALSE);
    }
    gain = settings.strength;
    SendDlgItemMessageW(window, IDC_FEEDBACK_STRENGTH, TBM_SETRANGE, TRUE, MAKELONG(gainStep, maxFeedbackGainPercent));
    SendDlgItemMessageW(window, IDC_FEEDBACK_STRENGTH, TBM_SETLINESIZE, 0, gainStep);
    SendDlgItemMessageW(window, IDC_FEEDBACK_STRENGTH, TBM_SETPAGESIZE, 0, gainStep * 4);
    // Preserve an existing value between steps until the user moves the slider.
    SendDlgItemMessageW(window, IDC_FEEDBACK_STRENGTH, TBM_SETPOS, TRUE, snapGain(gain));
    displayGain(window);
    addToggle(window, IDC_FEEDBACK_IMPACTS, tr(Text::DlgFeedbackImpactsDisabled), tr(Text::DlgFeedbackImpactsEnabled), settings.impacts);
    addToggle(window, IDC_FEEDBACK_DIRECTION, tr(Text::DlgFeedbackNormal), tr(Text::DlgFeedbackInverted), settings.invert);
    updateControls(window);
}

void FeedbackDialog::updateControls(HWND window) const
{
    const bool enabled = state == Compatibility::supported
        && SendDlgItemMessageW(window, IDC_FEEDBACK_ENABLE, CB_GETCURSEL, 0, 0) == 1;
    for (const int id : {IDC_FEEDBACK_STRENGTH, IDC_FEEDBACK_IMPACTS, IDC_FEEDBACK_DIRECTION})
        EnableWindow(GetDlgItem(window, id), enabled);
}

void FeedbackDialog::displayGain(HWND window) const
{
    const auto value = std::to_wstring(gain) + tr(Text::mainPercentSuffix);
    SetDlgItemTextW(window, IDC_FEEDBACK_STRENGTH_VALUE, value.c_str());
}

void FeedbackDialog::selectGain(HWND window)
{
    const auto position = static_cast<int>(SendDlgItemMessageW(window, IDC_FEEDBACK_STRENGTH, TBM_GETPOS, 0, 0));
    gain = snapGain(position);
    if (gain != position)
        SendDlgItemMessageW(window, IDC_FEEDBACK_STRENGTH, TBM_SETPOS, TRUE, gain);
    displayGain(window);
}

void FeedbackDialog::save(HWND window) const
{
    if (isTemporaryProfileDirectory(file.parent_path())) {
        MessageBoxW(window, tr(Text::DlgFeedbackExtractBeforeSaving), tr(Text::DlgFeedbackTitle), MB_OK | MB_ICONWARNING);
        return;
    }
    const auto selected = [&](int id) { return SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0); };

    const FeedbackSettings settings{
        state == Compatibility::supported && selected(IDC_FEEDBACK_ENABLE) == 1,
        gain,
        selected(IDC_FEEDBACK_IMPACTS) == 1,
        selected(IDC_FEEDBACK_DIRECTION) == 1,
    };
    std::string error;
    const std::array checks{ProfileFileCheck{profileFilename, calibrationRevision},
                            ProfileFileCheck{feedbackFilename, revision}};
    const auto result = savePresetSettings(root, preset, checks, [&] { return saveFeedbackSettings(file, settings, error); });
    if (result == PresetSaveResult::changed) {
        MessageBoxW(window, tr(Text::DlgFeedbackChanged), tr(Text::DlgFeedbackTitle), MB_OK | MB_ICONWARNING);
        return;
    }
    if (result != PresetSaveResult::saved) {
        MessageBoxW(window, tr(Text::DlgFeedbackSaveError),
                    tr(Text::DlgFeedbackTitle), MB_OK | MB_ICONERROR);
        return;
    }
    EndDialog(window, IDOK);
}

INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        SetWindowTextW(window, tr(Text::DlgFeedbackTitle));
        SetDlgItemTextW(window, IDC_FEEDBACK_ENABLE_LABEL, tr(Text::DlgFeedbackEnable));
        SetDlgItemTextW(window, IDC_FEEDBACK_STRENGTH_LABEL, tr(Text::DlgFeedbackStrength));
        SetDlgItemTextW(window, IDC_FEEDBACK_STRENGTH, tr(Text::DlgFeedbackGainSlider));
        SetDlgItemTextW(window, IDC_FEEDBACK_IMPACTS_LABEL, tr(Text::DlgFeedbackImpacts));
        SetDlgItemTextW(window, IDC_FEEDBACK_DIRECTION_LABEL, tr(Text::DlgFeedbackDirection));
        SetDlgItemTextW(window, IDC_MODAL_INTRO, tr(Text::DlgFeedbackIntro));
        SetDlgItemTextW(window, IDOK, tr(Text::DlgSave));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, tr(Text::DlgFeedbackHeading), IDC_FEEDBACK_STATUS, 0, {IDOK});
        reinterpret_cast<FeedbackDialog*>(lParam)->initialise(window);
        return TRUE;
    }
    auto* dialog = reinterpret_cast<FeedbackDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog)
        return FALSE;
    if (message == WM_NOTIFY && lParam && !ui::isHighContrast()) {
        const auto& notification = *reinterpret_cast<const NMHDR*>(lParam);
        if (notification.idFrom == IDC_FEEDBACK_STRENGTH && notification.code == NM_CUSTOMDRAW) {
            const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
            if (draw.dwDrawStage == CDDS_PREPAINT) {
                paintGainSlider(draw);
                SetWindowLongPtrW(window, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
        }
    }
    if (message == WM_HSCROLL && reinterpret_cast<HWND>(lParam) == GetDlgItem(window, IDC_FEEDBACK_STRENGTH)) {
        if (LOWORD(wParam) != TB_ENDTRACK)
            dialog->selectGain(window);
        return TRUE;
    }
    if (message != WM_COMMAND)
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

bool editForceFeedback(HWND owner, const std::filesystem::path& root, const ProfilePreset& preset,
                       const std::optional<ProfileFileRevision>& calibrationRevision,
                       const DeviceInfo* steeringDevice, std::size_t steeringAxis)
{
    if (!preset.directory.is_absolute())
        return false;
    const auto file = preset.directory / feedbackFilename;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    FeedbackDialog dialog{root, preset, calibrationRevision, file, profileFileRevision(file),
                          compatibility(steeringDevice, steeringAxis)};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FORCE_FEEDBACK), owner,
                                        dialogProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgFeedbackOpenError), tr(Text::DlgFeedbackTitle), MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
