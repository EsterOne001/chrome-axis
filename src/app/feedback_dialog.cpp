#include "chrome_axis/feedback_dialog.hpp"

#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/feedback.hpp"
#include "app_text.hpp"
#include "device_inventory.hpp"
#include "dialog_theme.hpp"
#include "feedback_editor.hpp"
#include "ui_theme.hpp"

#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
namespace chrome_axis {
namespace {

constexpr int gainStep = 5;
constexpr UINT_PTR feedbackProbeTimer = 1;

struct GainSlider {
    int control;
    int label;
    int minimum;
    int maximum;
};

constexpr GainSlider overall{IDC_FEEDBACK_STRENGTH, IDC_FEEDBACK_STRENGTH_VALUE, gainStep, maxFeedbackGainPercent};
constexpr GainSlider centering{IDC_FEEDBACK_CENTERING, IDC_FEEDBACK_CENTERING_VALUE,
                               minFeedbackCenteringPercent, maxFeedbackCenteringPercent};
constexpr GainSlider impactStrength{IDC_FEEDBACK_IMPACT_STRENGTH, IDC_FEEDBACK_IMPACT_STRENGTH_VALUE,
                                    0, maxFeedbackImpactPercent};

constexpr GainSlider lowSpeed{IDC_FEEDBACK_LOW_SPEED, IDC_FEEDBACK_LOW_SPEED_VALUE,
                              0, maxFeedbackTuningPercent};
constexpr GainSlider damping{IDC_FEEDBACK_DAMPING, IDC_FEEDBACK_DAMPING_VALUE,
                              0, maxFeedbackTuningPercent};

enum class Compatibility { noCalibration, checking, disconnected, unverified, unsupported, supported };

const wchar_t* compatibilityText(Compatibility state)
{
    switch (state) {
    case Compatibility::noCalibration:
        return tr(Text::DlgFeedbackNoCalibration);
    case Compatibility::checking:
        return tr(Text::DlgFeedbackChecking);
    case Compatibility::disconnected:
        return tr(Text::DlgFeedbackDisconnected);
    case Compatibility::unverified:
        return tr(Text::DlgFeedbackUnverified);
    case Compatibility::unsupported:
        return tr(Text::DlgFeedbackUnsupported);
    case Compatibility::supported:
        return tr(Text::DlgFeedbackSupported);
    }
    return L"";
}

const wchar_t* feedbackReadText(FeedbackEditorStatus state)
{
    switch (state) {
    case FeedbackEditorStatus::ready: return tr(Text::DlgFeedbackIntro);
    case FeedbackEditorStatus::invalid: return tr(Text::DlgFeedbackInvalid);
    case FeedbackEditorStatus::locked: return tr(Text::DlgFeedbackLocked);
    case FeedbackEditorStatus::unreadable: return tr(Text::DlgFeedbackUnreadable);
    case FeedbackEditorStatus::changed: return tr(Text::DlgFeedbackChanged);
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

int snapGain(const GainSlider& slider, int percent)
{
    return std::clamp((percent + gainStep / 2) / gainStep * gainStep, slider.minimum, slider.maximum);
}

void showGain(HWND window, const GainSlider& slider, int gain)
{
    const auto value = std::to_wstring(gain) + tr(Text::mainPercentSuffix);
    SetDlgItemTextW(window, slider.label, value.c_str());
}

void initialiseGain(HWND window, const GainSlider& slider, int gain)
{
    SendDlgItemMessageW(window, slider.control, TBM_SETRANGE, TRUE, MAKELONG(slider.minimum, slider.maximum));
    SendDlgItemMessageW(window, slider.control, TBM_SETLINESIZE, 0, gainStep);
    SendDlgItemMessageW(window, slider.control, TBM_SETPAGESIZE, 0, gainStep * 4);
    // Preserve existing values between steps until the user moves the slider.
    SendDlgItemMessageW(window, slider.control, TBM_SETPOS, TRUE, snapGain(slider, gain));
    showGain(window, slider, gain);
}

int selectGain(HWND window, const GainSlider& slider)
{
    const auto position = static_cast<int>(SendDlgItemMessageW(window, slider.control, TBM_GETPOS, 0, 0));
    const int gain = snapGain(slider, position);
    if (gain != position)
        SendDlgItemMessageW(window, slider.control, TBM_SETPOS, TRUE, gain);
    showGain(window, slider, gain);
    return gain;
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
    std::string steeringDevice;
    std::size_t steeringAxis;
    FeedbackEditorRead original;
    std::optional<DeviceInventory> inventory;
    Compatibility state{Compatibility::noCalibration};
    int gain{};
    int centeringGain{};
    int impactGain{};
    int lowSpeedGain{};
    int dampingGain{};
    std::optional<int> suggestedGain;
    std::uint64_t continuousClips{};
    std::uint64_t impactClips{};

    FeedbackSettings draft(HWND window) const;
    bool compatibilityUnknown() const noexcept;
    bool canSave(const FeedbackSettings& settings) const noexcept;
    bool suggestionCurrent(const FeedbackSettings& settings) const noexcept;
    void showSuggestion(HWND window, bool available) const;
    void updateCompatibility(HWND window) const;
    void pollCompatibility(HWND window);

    void initialise(HWND window);
    void updateControls(HWND window) const;
    void save(HWND window) const;
};

void FeedbackDialog::initialise(HWND window)
{
    original = readFeedbackForEdit(file);
    const auto settings = original.settings.value_or(FeedbackSettings{});
    if (steeringAxis < axisCount && !steeringDevice.empty()) {
        inventory.emplace(steeringDevice, steeringAxis);
        inventory->refresh();
        if (SetTimer(window, feedbackProbeTimer, 100, nullptr)) {
            state = Compatibility::checking;
        } else {
            inventory.reset();
            state = Compatibility::unverified;
        }
    }
    addToggle(window, IDC_FEEDBACK_ENABLE, tr(Text::DlgFeedbackDisabled), tr(Text::DlgFeedbackEnabled), settings.enabled);
    gain = settings.strength;
    centeringGain = settings.centering;
    impactGain = settings.impactStrength;
    initialiseGain(window, overall, gain);
    initialiseGain(window, centering, centeringGain);
    initialiseGain(window, impactStrength, impactGain);
    lowSpeedGain = settings.lowSpeedEffort;
    dampingGain = settings.damping;
    initialiseGain(window, lowSpeed, lowSpeedGain);
    initialiseGain(window, damping, dampingGain);
    const auto gameRoot = installedGameRoot(root.parent_path());
    if (settings.enabled && !gameRoot.empty()) {
        std::string error;
        const auto profile = loadProfile(preset.directory / profileFilename, error);
        const auto capture = DiagnosticReader(gameRoot).read().performance;
        const auto name = std::filesystem::path(preset.name).u8string();
        const std::string_view profileName(reinterpret_cast<const char*>(name.data()), name.size());
        if (profile && matchesFeedbackMeasurement(capture, settings, *profile, profileName)) {
            // Reserve headroom for impacts and limit one recommendation to a modest increase.
            const float scale = std::min(1.25f, 0.88f / capture.peakContinuousForce);
            suggestedGain = snapGain(overall, static_cast<int>(std::lround(settings.strength * scale)));
            continuousClips = capture.clippedSamples - std::min(capture.clippedSamples, capture.impactClippedSamples);
            impactClips = capture.impactClippedSamples;
        }
    }
    addToggle(window, IDC_FEEDBACK_IMPACTS, tr(Text::DlgFeedbackImpactsDisabled), tr(Text::DlgFeedbackImpactsEnabled), settings.impacts);
    addToggle(window, IDC_FEEDBACK_DIRECTION, tr(Text::DlgFeedbackNormal), tr(Text::DlgFeedbackInverted), settings.invert);
    updateCompatibility(window);
    updateControls(window);
}

bool FeedbackDialog::compatibilityUnknown() const noexcept
{
    return state == Compatibility::checking || state == Compatibility::unverified;
}

bool FeedbackDialog::canSave(const FeedbackSettings& settings) const noexcept
{
    if (!original.canSave())
        return false;
    return !compatibilityUnknown() || (!settings.enabled &&
        (original.status == FeedbackEditorStatus::invalid || (original.settings && original.settings->enabled)));
}

FeedbackSettings FeedbackDialog::draft(HWND window) const
{
    const auto selected = [window](int id) { return SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0) == 1; };
    return {selected(IDC_FEEDBACK_ENABLE), gain, selected(IDC_FEEDBACK_IMPACTS), selected(IDC_FEEDBACK_DIRECTION),
        centeringGain, impactGain, lowSpeedGain, dampingGain};
}

bool FeedbackDialog::suggestionCurrent(const FeedbackSettings& settings) const noexcept
{
    return state == Compatibility::supported && suggestedGain && original.settings == settings;
}

void FeedbackDialog::updateCompatibility(HWND window) const
{
    SetDlgItemTextW(window, IDC_FEEDBACK_STATUS, compatibilityText(state));
    const bool confirmDisable = original.canSave() && !compatibilityUnknown() && state != Compatibility::supported &&
        original.settings && original.settings->enabled;
    if (confirmDisable)
        SendDlgItemMessageW(window, IDC_FEEDBACK_ENABLE, CB_SETCURSEL, 0, 0);
    SetDlgItemTextW(window, IDC_MODAL_INTRO,
        confirmDisable ? tr(Text::DlgFeedbackDisableProfile) : feedbackReadText(original.status));
}

void FeedbackDialog::pollCompatibility(HWND window)
{
    const auto result = inventory ? inventory->current() : nullptr;
    if (!result || (!result->available && state == Compatibility::unverified))
        return;
    if (result->available)
        KillTimer(window, feedbackProbeTimer);
    const auto device = std::ranges::find(result->devices, steeringDevice, &DeviceInfo::id);
    state = !result->available ? Compatibility::unverified
        : device == result->devices.end() ? Compatibility::disconnected
        : device->forceFeedbackAxes[steeringAxis] ? Compatibility::supported : Compatibility::unsupported;
    updateCompatibility(window);
    updateControls(window);
}

void FeedbackDialog::showSuggestion(HWND window, bool available) const
{
    if (!available) {
        SetDlgItemTextW(window, IDC_FEEDBACK_SUGGESTION, tr(Text::DlgFeedbackNoSuggestion));
        return;
    }
    wchar_t label[220]{};
    swprintf_s(label, tr(Text::DlgFeedbackSuggestion), gain, *suggestedGain,
               static_cast<unsigned long long>(continuousClips), static_cast<unsigned long long>(impactClips));
    SetDlgItemTextW(window, IDC_FEEDBACK_SUGGESTION, label);
}

void FeedbackDialog::updateControls(HWND window) const
{
    const bool editable = original.canSave() && state == Compatibility::supported;
    const auto settings = draft(window);
    const bool enabled = editable && settings.enabled;
    const bool disableUnverified = original.canSave() && compatibilityUnknown() && original.settings && original.settings->enabled;
    EnableWindow(GetDlgItem(window, IDOK), canSave(settings));
    EnableWindow(GetDlgItem(window, IDC_FEEDBACK_ENABLE), editable || disableUnverified);
    for (const int id : {overall.control, centering.control, lowSpeed.control, damping.control, IDC_FEEDBACK_IMPACTS, IDC_FEEDBACK_DIRECTION})
        EnableWindow(GetDlgItem(window, id), enabled);
    const bool currentAdvice = enabled && suggestionCurrent(settings);
    showSuggestion(window, currentAdvice);
    EnableWindow(GetDlgItem(window, IDC_FEEDBACK_USE_SUGGESTION), currentAdvice && gain != *suggestedGain);
    EnableWindow(GetDlgItem(window, impactStrength.control), enabled && settings.impacts);
}

void FeedbackDialog::save(HWND window) const
{
    auto settings = draft(window);
    if (!canSave(settings))
        return;
    if (isTemporaryProfileDirectory(file.parent_path())) {
        MessageBoxW(window, tr(Text::DlgFeedbackExtractBeforeSaving), tr(Text::DlgFeedbackTitle), MB_OK | MB_ICONWARNING);
        return;
    }
    settings.enabled = state == Compatibility::supported && settings.enabled;
    FeedbackEditorStatus problem{};
    const auto result = saveEditedFeedback(root, preset, calibrationRevision, original, settings, problem);
    if (result == PresetSaveResult::changed) {
        MessageBoxW(window, tr(Text::DlgFeedbackChanged), tr(Text::DlgFeedbackTitle), MB_OK | MB_ICONWARNING);
        return;
    }
    if (result != PresetSaveResult::saved) {
        const bool readFailed = problem == FeedbackEditorStatus::locked || problem == FeedbackEditorStatus::unreadable;
        MessageBoxW(window, readFailed ? feedbackReadText(problem) : tr(Text::DlgFeedbackSaveError),
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
        SetDlgItemTextW(window, IDC_FEEDBACK_CENTERING_LABEL, tr(Text::DlgFeedbackCentering));
        SetDlgItemTextW(window, IDC_FEEDBACK_CENTERING, tr(Text::DlgFeedbackCenteringSlider));
        SetDlgItemTextW(window, IDC_FEEDBACK_IMPACTS_LABEL, tr(Text::DlgFeedbackImpacts));
        SetDlgItemTextW(window, IDC_FEEDBACK_IMPACT_STRENGTH_LABEL, tr(Text::DlgFeedbackImpactStrength));
        SetDlgItemTextW(window, IDC_FEEDBACK_IMPACT_STRENGTH, tr(Text::DlgFeedbackImpactStrengthSlider));
        SetDlgItemTextW(window, IDC_FEEDBACK_LOW_SPEED_LABEL, tr(Text::DlgFeedbackLowSpeed));
        SetDlgItemTextW(window, IDC_FEEDBACK_LOW_SPEED, tr(Text::DlgFeedbackLowSpeed));
        SetDlgItemTextW(window, IDC_FEEDBACK_DAMPING_LABEL, tr(Text::DlgFeedbackDamping));
        SetDlgItemTextW(window, IDC_FEEDBACK_DAMPING, tr(Text::DlgFeedbackDamping));
        SetDlgItemTextW(window, IDC_FEEDBACK_USE_SUGGESTION, tr(Text::DlgFeedbackUseSuggestion));
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
    if (message == WM_TIMER && wParam == feedbackProbeTimer) {
        dialog->pollCompatibility(window);
        return TRUE;
    }
    if (message == WM_NOTIFY && lParam && !ui::isHighContrast()) {
        const auto& notification = *reinterpret_cast<const NMHDR*>(lParam);
        if ((notification.idFrom == overall.control || notification.idFrom == centering.control ||
             notification.idFrom == impactStrength.control || notification.idFrom == lowSpeed.control ||
             notification.idFrom == damping.control) &&
            notification.code == NM_CUSTOMDRAW) {
            const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
            if (draw.dwDrawStage == CDDS_PREPAINT) {
                paintGainSlider(draw);
                SetWindowLongPtrW(window, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
        }
    }
    if (message == WM_HSCROLL && lParam && LOWORD(wParam) != TB_ENDTRACK) {
        const int control = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        const auto change = [&](const GainSlider& slider, int& gain) {
            gain = selectGain(window, slider);
            dialog->updateControls(window);
            return TRUE;
        };
        if (control == overall.control)
            return change(overall, dialog->gain);
        if (control == centering.control)
            return change(centering, dialog->centeringGain);
        if (control == impactStrength.control)
            return change(impactStrength, dialog->impactGain);
        if (control == lowSpeed.control)
            return change(lowSpeed, dialog->lowSpeedGain);
        if (control == damping.control)
            return change(damping, dialog->dampingGain);
    }
    if (message != WM_COMMAND)
        return FALSE;
    switch (LOWORD(wParam)) {
    case IDC_FEEDBACK_ENABLE:
    case IDC_FEEDBACK_IMPACTS:
    case IDC_FEEDBACK_DIRECTION:
        if (HIWORD(wParam) == CBN_SELCHANGE)
            dialog->updateControls(window);
        return TRUE;
    case IDC_FEEDBACK_USE_SUGGESTION:
        if (dialog->suggestedGain && IsWindowEnabled(GetDlgItem(window, IDC_FEEDBACK_USE_SUGGESTION))) {
            dialog->gain = *dialog->suggestedGain;
            initialiseGain(window, overall, dialog->gain);
            dialog->updateControls(window);
        }
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
                       std::string_view steeringDevice, std::size_t steeringAxis)
{
    if (!preset.directory.is_absolute())
        return false;
    const auto file = preset.directory / feedbackFilename;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    FeedbackDialog dialog{root, preset, calibrationRevision, file, std::string(steeringDevice), steeringAxis};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FORCE_FEEDBACK), owner,
                                        dialogProc, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1)
        MessageBoxW(owner, tr(Text::DlgFeedbackOpenError), tr(Text::DlgFeedbackTitle), MB_OK | MB_ICONERROR);
    return result == IDOK;
}

} // namespace chrome_axis
