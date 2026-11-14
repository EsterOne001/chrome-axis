#include "chrome_axis/accessories.hpp"
#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/calibration.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/feedback_dialog.hpp"
#include "chrome_axis/presets.hpp"
#include "chrome_axis/profiles_dialog.hpp"
#include "chrome_axis/simulation.hpp"
#include "app_text.hpp"
#include "calibration_guide.hpp"
#include "diagnostics_dialog.hpp"
#include "ui_theme.hpp"

#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using chrome_axis::Language;
using chrome_axis::Text;
using chrome_axis::tr;
using namespace chrome_axis::ui;

constexpr wchar_t windowClass[] = L"ChromeAxisCalibration";
constexpr wchar_t title[] = L"Chrome Axis";
constexpr wchar_t nexusModUrl[] = L"https://www.nexusmods.com/cyberpunk2077/mods/38104";
constexpr UINT_PTR inputTimer = 1;
constexpr UINT_PTR deviceTimer = 2;
constexpr int contentWidthLimit = 1120;
constexpr int languageButtonSize = 36;
constexpr int languageButtonSpace = languageButtonSize + 12;
constexpr std::array commands{Text::mainSteering, Text::mainThrottle, Text::mainBrake};
constexpr std::array deviceLabels{Text::mainWheel, Text::mainThrottle, Text::mainBrake};
constexpr std::array sharedDeviceLabels{Text::mainSameWheel, Text::mainSameThrottle};
constexpr std::array keyLabels{Text::mainSteeringKeys, Text::mainThrottleKeys, Text::mainBrakeKeys};
constexpr std::string_view sameAsPrevious = "*";

enum Command {
    deviceComboFirst = 100, throttleCombo, brakeCombo, refreshDevices, configureAccessories,
    startCalibration, measure, cancelCalibration, chooseAxis,
    saveProfile, reloadProfile, openProfileFolder, closeApp, aboutApp, hardwareTab, simulationTab, toggleSimulation,
    toggleUsb, selectProfile, configureFeedback, automaticLanguage, frenchLanguage, englishLanguage,
    frenchFlag, englishFlag, chooseCalibrationScope, showDiagnostics, previousCalibration,
    backupProfiles, restoreProfiles, openModInfo, reassignDevice,
    deadzoneFirst = 200, meterFirst = 210, retryAxisFirst = 220
};

using CalibrationScope = std::array<bool, commands.size()>;
constexpr CalibrationScope allChannels{true, true, true};

struct LanguageOption {
    Command command;
    Language language;
    Text label;
};

constexpr std::array languageOptions{
    LanguageOption{automaticLanguage, Language::automatic, Text::mainMenuAutomatic},
    LanguageOption{frenchLanguage, Language::french, Text::mainMenuFrench},
    LanguageOption{englishLanguage, Language::english, Text::mainMenuEnglish}
};

enum class Position { Neutral, Negative, Positive };

struct Step {
    std::size_t channel;
    Position position;
    Text instruction;
    Text heading;
    CalibrationPose pose;
};

constexpr std::array calibrationSteps{
    Step{0, Position::Neutral, Text::mainCenterWheel, Text::guideCenter, CalibrationPose::wheelCenter},
    Step{0, Position::Negative, Text::mainTurnLeft, Text::guideLeft, CalibrationPose::wheelLeft},
    Step{0, Position::Positive, Text::mainTurnRight, Text::guideRight, CalibrationPose::wheelRight},
    Step{1, Position::Neutral, Text::mainReleasePedalsCenterWheel, Text::guideReleaseThrottle, CalibrationPose::throttleRest},
    Step{1, Position::Positive, Text::mainPressThrottle, Text::guidePressThrottle, CalibrationPose::throttlePressed},
    Step{2, Position::Neutral, Text::mainReleasePedals, Text::guideReleaseBrake, CalibrationPose::brakeRest},
    Step{2, Position::Positive, Text::mainPressBrake, Text::guidePressBrake, CalibrationPose::brakePressed}
};

constexpr std::array retryAxisLabels{Text::guideRetrySteering, Text::guideRetryThrottle, Text::guideRetryBrake};

struct AxisRetry {
    std::size_t first;
    std::size_t end;
    std::array<chrome_axis::AxisCapture, calibrationSteps.size()> positions;
};

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const auto length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (size == 0)
        return tr(Text::mainTextUnavailable);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size);
    return result;
}

bool hasAxes(const chrome_axis::DeviceInfo& device)
{
    return std::ranges::any_of(device.axes, [](bool present) { return present; });
}

bool deployedProfileDirectory(const std::filesystem::path& directory)
{
    return chrome_axis::isInstalledPluginDirectory(directory.parent_path())
        && !chrome_axis::isTemporaryProfileDirectory(directory);
}

const wchar_t* pluginInstallAdvice(const std::filesystem::path& pluginDirectory, Text unknownGameText)
{
    if (!chrome_axis::installedGameRoot(pluginDirectory).empty()) {
        if (const auto* missing = chrome_axis::missingPluginFilesText(chrome_axis::pluginFiles(pluginDirectory)))
            return missing;
    }
    return tr(unknownGameText);
}

std::wstring backupFolderName()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::format(L"Chrome Axis - {:04}-{:02}-{:02}_{:02}-{:02}-{:02}", now.wYear, now.wMonth,
                       now.wDay, now.wHour, now.wMinute, now.wSecond);
}

std::wstring formatDeadzone(float value)
{
    const auto tenths = std::lround(1000 * value);
    auto result = std::to_wstring(tenths / 10);
    if (tenths % 10)
        result += tr(Text::mainDecimalSeparator) + std::to_wstring(tenths % 10);
    return result;
}

std::optional<float> readDeadzone(HWND edit)
{
    wchar_t text[6]{};
    const int length = GetWindowTextW(edit, text, static_cast<int>(std::size(text)));
    char number[6]{};
    for (int i = 0; i < length; ++i) {
        const auto ch = text[i];
        if ((ch < L'0' || ch > L'9') && ch != L',' && ch != L'.')
            return std::nullopt;
        number[i] = ch == L',' ? '.' : static_cast<char>(ch);
    }
    float percent{};
    const auto end = number + length;
    const auto result = std::from_chars(number, end, percent);
    if (result.ec != std::errc{} || result.ptr != end || percent < 0.5f || percent > 25.0f)
        return std::nullopt;
    return percent / 100.0f;
}

void setText(HWND control, const std::wstring& text)
{
    wchar_t current[512]{};
    GetWindowTextW(control, current, static_cast<int>(std::size(current)));
    if (text != current)
        SetWindowTextW(control, text.c_str());
}

void addChoice(HWND combo, const std::wstring& text)
{
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

class App {
public:
    explicit App(std::filesystem::path directory)
        : directory_(std::move(directory)), gameRoot_(chrome_axis::installedGameRoot(directory_.parent_path())),
          publisher_(gameRoot_), simulationDiagnostics_(gameRoot_) {}
    ~App()
    {
        for (const auto font : fonts_)
            DeleteObject(font);
        DeleteObject(backgroundBrush_);
        DeleteObject(panelBrush_);
        DeleteObject(fieldBrush_);
    }

    LRESULT handle(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        switch (message) {
        case WM_CREATE:
            window_ = window;
            return create() ? 0 : -1;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED)
                suspendHardware();
            else if (status_)
                layout();
            return 0;
        case WM_ACTIVATEAPP:
            if (!wp)
                suspendHardware();
            return 0;
        case WM_SETTINGCHANGE:
            updateAnimationPreference();
            updateCalibrationMotion(GetForegroundWindow() == window_ && !IsIconic(window_));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            RECT minimum{0, 0, px(736), px(622)};
            AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_CONTROLPARENT, dpi_);
            limits->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const auto& rect = *reinterpret_cast<const RECT*>(lp);
            updateDpiResources();
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left,
                         rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            layout();
            return 0;
        }
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                if (!SetTimer(window, deviceTimer, 300, nullptr))
                    deviceRefreshPending_ = true;
            }
            return TRUE;
        case WM_TIMER:
            if (wp == deviceTimer) {
                KillTimer(window, deviceTimer);
                deviceRefreshPending_ = true;
            } else if (wp == inputTimer) {
                tick();
            }
            return 0;
        case WM_COMMAND:
            action(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wp) == window) {
                contextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            break;
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            const auto dc = reinterpret_cast<HDC>(wp);
            const auto control = reinterpret_cast<HWND>(lp);
            if (isHighContrast()) {
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            const bool footer = control == status_ || control == profileName_;
            const bool isField = message != WM_CTLCOLORSTATIC || std::find(zones_.begin(), zones_.end(), control) != zones_.end();
            const bool heading = control == devicesTitle_ || control == axesTitle_ || control == calibrationTitle_;
            const bool muted = footer || control == hardware_ || control == instruction_ || !IsWindowEnabled(control);
            const bool value = std::find(values_.begin(), values_.end(), control) != values_.end();
            const bool guideNotice = control == guideNotice_ && (capture_ || !calibrationMessage_.empty());
            SetTextColor(dc, heading ? yellow : guideNotice || value ? cyan : muted ? secondary : ink);
            SetBkColor(dc, footer ? background : isField ? field : panel);
            return reinterpret_cast<LRESULT>(footer ? backgroundBrush_ : isField ? fieldBrush_ : panelBrush_);
        }
        case WM_MEASUREITEM: {
            auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (item.CtlType == ODT_COMBOBOX) {
                item.itemHeight = static_cast<UINT>(px(24));
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (item.CtlID >= meterFirst && item.CtlID < meterFirst + commands.size()) {
                paintMeter(item, item.CtlID - meterFirst);
                return TRUE;
            }
            if (item.CtlType == ODT_BUTTON) {
                paintButton(item);
                return TRUE;
            }
            if (item.CtlType == ODT_COMBOBOX) {
                paintCombo(item);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            if (!dirty_ || MessageBoxW(window, tr(Text::mainDiscardOnClose),
                                       title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
                DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, inputTimer);
            KillTimer(window, deviceTimer);
            input_.reset();
            publisher_.disable();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

private:
    HWND child(const wchar_t* kind, const wchar_t* text, DWORD style = 0, int id = 0, DWORD ex = 0)
    {
        const auto control = CreateWindowExW(ex, kind, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
            0, 0, 1, 1, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        return control;
    }

    HWND label(const wchar_t* text) { return child(L"STATIC", text, SS_NOPREFIX); }
    HWND button(const wchar_t* text, int id) { return child(L"BUTTON", text, WS_TABSTOP | BS_OWNERDRAW, id); }
    HWND combo(int id) { return child(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, id); }
    int px(int number) const { return MulDiv(number, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI); }

    std::pair<int, int> contentBounds(LONG pixelWidth) const
    {
        const int clientWidth = MulDiv(pixelWidth, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi_));
        const int width = std::min(clientWidth, contentWidthLimit);
        return {(clientWidth - width) / 2, width};
    }

    void redrawControls()
    {
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    bool create()
    {
        dpi_ = GetDpiForWindow(window_);
        backgroundBrush_ = CreateSolidBrush(background);
        panelBrush_ = CreateSolidBrush(panel);
        fieldBrush_ = CreateSolidBrush(field);
        const HMENU menu = CreateMenu();
        profileMenu_ = CreatePopupMenu();
        AppendMenuW(profileMenu_, MF_STRING, saveProfile, tr(Text::mainMenuSave));
        AppendMenuW(profileMenu_, MF_STRING, reloadProfile, tr(Text::mainMenuReload));
        AppendMenuW(profileMenu_, MF_STRING, selectProfile, tr(Text::mainMenuSelectProfile));
        AppendMenuW(profileMenu_, MF_STRING, openProfileFolder, tr(Text::mainMenuOpenProfiles));
        AppendMenuW(profileMenu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(profileMenu_, MF_STRING, backupProfiles, tr(Text::mainMenuBackupProfiles));
        AppendMenuW(profileMenu_, MF_STRING, restoreProfiles, tr(Text::mainMenuRestoreProfiles));
        AppendMenuW(profileMenu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(profileMenu_, MF_STRING, closeApp, tr(Text::mainMenuQuit));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profileMenu_), tr(Text::mainMenuProfile));

        deviceMenu_ = CreatePopupMenu();
        AppendMenuW(deviceMenu_, MF_STRING, refreshDevices, tr(Text::mainMenuRefresh));
        AppendMenuW(deviceMenu_, MF_STRING, startCalibration, tr(Text::mainMenuCalibrate));
        AppendMenuW(deviceMenu_, MF_STRING, chooseCalibrationScope, tr(Text::mainMenuCalibrationScope));
        AppendMenuW(deviceMenu_, MF_STRING, reassignDevice, tr(Text::mainMenuReassignDevice));
        AppendMenuW(deviceMenu_, MF_STRING, configureAccessories, tr(Text::mainMenuAccessories));
        AppendMenuW(deviceMenu_, MF_STRING, configureFeedback, tr(Text::mainMenuFeedback));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(deviceMenu_), tr(Text::mainMenuDevices));
        helpMenu_ = CreatePopupMenu();
        languageMenu_ = CreatePopupMenu();
        for (const auto& option : languageOptions)
            AppendMenuW(languageMenu_, MF_STRING, option.command, tr(option.label));
        const auto selectedLanguage = std::ranges::find(languageOptions, chrome_axis::languageChoice(),
            &LanguageOption::language);
        CheckMenuRadioItem(languageMenu_, automaticLanguage, englishLanguage, selectedLanguage->command, MF_BYCOMMAND);
        AppendMenuW(helpMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu_), tr(Text::mainMenuLanguage));
        AppendMenuW(helpMenu_, MF_STRING, showDiagnostics, tr(Text::DlgDiagnosticsMenu));
        AppendMenuW(helpMenu_, MF_STRING, openModInfo, tr(Text::mainMenuModInfo));
        AppendMenuW(helpMenu_, MF_STRING, aboutApp, tr(Text::mainMenuAbout));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu_), tr(Text::mainMenuHelp));
        SetMenu(window_, menu);

        hardwareTab_ = button(tr(Text::mainUsbHardware), hardwareTab);
        simulationTab_ = button(tr(Text::mainSimulation), simulationTab);
        frenchFlag_ = button(L"Français", frenchFlag);
        englishFlag_ = button(L"English", englishFlag);
        devicesTitle_ = label(tr(Text::mainDevices));
        usbToggle_ = button(usbActionLabel(), toggleUsb);
        refresh_ = button(tr(Text::mainRefresh), refreshDevices);
        accessories_ = button(tr(Text::mainAccessories), configureAccessories);
        for (std::size_t i = 0; i < deviceLabels.size(); ++i) {
            deviceLabels_[i] = label(tr(deviceLabels[i]));
            deviceCombos_[i] = combo(deviceComboFirst + static_cast<int>(i));
        }
        hardware_ = label(tr(Text::mainScanning));
        simToggle_ = button(tr(Text::mainEnableInGame), toggleSimulation);
        simState_ = label(L"");
        simHint_ = label(tr(Text::mainSimHint));

        axesTitle_ = label(tr(Text::mainVehicleAxes));
        headings_ = {label(tr(Text::mainControl)), label(tr(Text::mainDetectedAxis)), label(tr(Text::mainDeadzone)), label(tr(Text::mainLocalPreview))};
        for (std::size_t i = 0; i < commands.size(); ++i) {
            names_[i] = label(tr(commands[i]));
            bindings_[i] = label(tr(Text::mainNeedsCalibration));
            zones_[i] = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                              deadzoneFirst + static_cast<int>(i), WS_EX_CLIENTEDGE);
            SendMessageW(zones_[i], EM_SETLIMITTEXT, 5, 0);
            meters_[i] = child(L"STATIC", L"", SS_OWNERDRAW, meterFirst + static_cast<int>(i));
            values_[i] = label(L"—");
            simKeys_[i] = label(tr(keyLabels[i]));
            retryAxes_[i] = button(tr(retryAxisLabels[i]), retryAxisFirst + static_cast<int>(i));
            ShowWindow(retryAxes_[i], SW_HIDE);
        }

        calibrationTitle_ = label(tr(Text::mainCalibration));
        diagnostics_ = button(tr(Text::DlgDiagnosticsButton), showDiagnostics);
        feedback_ = button(tr(Text::mainForceFeedback), configureFeedback);
        instruction_ = label(tr(Text::mainConnectToBegin));
        guideStep_ = label(L"");
        guideAction_ = label(L"");
        guideDevice_ = child(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS);
        guideNotice_ = label(L"");
        start_ = button(tr(Text::mainCalibrateAxes), startCalibration);
        calibrationScopeButton_ = button(tr(Text::mainCalibrationScope), chooseCalibrationScope);
        axisChoice_ = combo(chooseAxis);
        cancel_ = button(tr(Text::mainCancel), cancelCalibration);
        previous_ = button(tr(Text::guidePrevious), previousCalibration);
        measure_ = button(tr(Text::guideCapture), measure);
        simDirections_ = label(tr(Text::mainSimDirections));
        simArming_ = label(tr(Text::mainSimArming));
        profileName_ = child(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0);
        status_ = label(tr(Text::mainNoProfile));
        folder_ = button(tr(Text::mainProfilesFolder), openProfileFolder);
        select_ = button(tr(Text::mainSelectProfile), selectProfile);
        reload_ = button(tr(Text::mainReload), reloadProfile);
        save_ = button(tr(Text::mainSaveProfile), saveProfile);

        input_ = std::make_unique<chrome_axis::DirectInput>(window_);
        updateAnimationPreference();
        updateDpiResources();
        readProfile();
        scanDevices();
        applyMode();
        updateCalibrationView();
        layout();
        if (!SetTimer(window_, inputTimer, 30, nullptr)) {
            MessageBoxW(window_, tr(Text::mainInputStartFailed),
                title, MB_OK | MB_ICONERROR);
            return false;
        }
        return true;
    }

    void updateDpiResources()
    {
        for (const auto font : fonts_)
            DeleteObject(font);
        headerLogo_ = loadLogo(GetModuleHandleW(nullptr), px(headerLogoSize));
        fonts_ = makeFonts(window_, dpi_);
        EnumChildWindows(window_, [](HWND control, LPARAM font) -> BOOL {
            SendMessageW(control, WM_SETFONT, static_cast<WPARAM>(font), FALSE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(fonts_[0]));
        for (const auto control : {devicesTitle_, axesTitle_, calibrationTitle_, hardwareTab_, simulationTab_, guideAction_})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_[1]), FALSE);
        for (const auto control : deviceCombos_)
            SendMessageW(control, CB_SETITEMHEIGHT, 0, px(24));
        SendMessageW(axisChoice_, CB_SETITEMHEIGHT, 0, px(24));
        // The two native buttons share one square but keep independent focus and hit regions.
        const bool shaped = shapeLanguageButton(frenchFlag_, px(languageButtonSize), Flag::france)
            && shapeLanguageButton(englishFlag_, px(languageButtonSize), Flag::unitedKingdom);
        ShowWindow(frenchFlag_, shaped ? SW_SHOWNA : SW_HIDE);
        ShowWindow(englishFlag_, shaped ? SW_SHOWNA : SW_HIDE);
    }

    void layout()
    {
        RECT rect{};
        GetClientRect(window_, &rect);
        const auto [left, width] = contentBounds(rect.right);
        struct Placement {
            HWND control;
            int x, y, width, height;
        };
        std::vector<Placement> placements;
        placements.reserve(65);
        const auto place = [&](HWND control, int x, int y, int w, int h) {
            if (control)
                placements.push_back({control, px(left + x), px(y), px(w), px(h)});
        };

        place(hardwareTab_, width - 314 - languageButtonSpace, 26, 148, 36);
        place(simulationTab_, width - 156 - languageButtonSpace, 26, 132, 36);
        for (const auto button : {frenchFlag_, englishFlag_})
            place(button, width - 24 - languageButtonSize, 26, languageButtonSize, languageButtonSize);
        const int comboWidth = (width - 112) / 3;
        place(devicesTitle_, 42, 110, width - 542, 24);
        place(usbToggle_, width - 492, 106, 200, 29);
        place(accessories_, width - 282, 106, 120, 29);
        place(refresh_, width - 152, 106, 110, 29);
        for (std::size_t i = 0; i < deviceCombos_.size(); ++i) {
            const int x = 42 + static_cast<int>(i) * (comboWidth + 14);
            place(deviceLabels_[i], x, 142, comboWidth, 20);
            place(deviceCombos_[i], x, 165, comboWidth, 180);
        }
        place(hardware_, 42, 198, width - 84, 21);
        place(simToggle_, width - 230, 123, 188, 35);
        place(simState_, 42, 150, width - 290, 22);
        place(simHint_, 42, 184, width - 84, 22);

        place(axesTitle_, 42, 247, 300, 24);
        constexpr std::array column{42, 184, 298, 434};
        constexpr std::array headingWidths{130, 110, 132, 130};
        const bool reviewing = reviewingCalibration();
        const int meterLeft = reviewing ? 220 : column[3];
        const int meterWidth = width - meterLeft - (reviewing ? 314 : 112);
        const int pixelWidth = px(meterWidth);
        meterSpans_ = {pixelWidth - pixelWidth / 2, pixelWidth, pixelWidth};
        for (std::size_t i = 0; i < column.size(); ++i)
            place(headings_[i], column[i], 278, headingWidths[i], 20);
        int reviewRow = 0;
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const int y = reviewing ? 300 + reviewRow * 44 : 303 + static_cast<int>(i) * 32;
            if (reviewing && calibrationScope_[i])
                ++reviewRow;
            place(names_[i], reviewing ? 46 : column[0], y + 3, 150, 22);
            place(bindings_[i], column[1], y + 3, 105, 22);
            place(simKeys_[i], column[1], y + 3, 226, 22);
            place(zones_[i], column[2], y, 62, 24);
            place(meters_[i], meterLeft, y + 8, meterWidth, 11);
            place(values_[i], width - (reviewing ? 302 : 102), y + 3, 64, 22);
            place(retryAxes_[i], width - 226, y - 2, 180, 32);
        }

        place(diagnostics_, width - 339, 428, 133, 29);
        place(feedback_, width - 196, 428, 154, 29);
        place(start_, 42, 491, 158, 29);
        place(calibrationScopeButton_, 210, 491, 180, 29);
        if (calibrating_) {
            place(calibrationTitle_, 46, 119, width - 270, 29);
            place(guideStep_, width - 194, 124, 148, 24);
            const int guideLeft = reviewing ? 46 : 280;
            const int guideWidth = width - guideLeft - 52;
            place(guideAction_, guideLeft, 196, guideWidth, 32);
            place(guideDevice_, guideLeft, 233, guideWidth, 25);
            place(instruction_, guideLeft, reviewing ? 238 : 270, guideWidth, 57);
            place(guideNotice_, guideLeft, 338, guideWidth, 68);
            place(axisChoice_, 280, 412, width - 332, 180);
            place(cancel_, 46, 468, 144, 38);
            place(previous_, width - 406, 468, 144, 38);
            place(measure_, width - 252, 468, 206, 38);
        } else {
            place(calibrationTitle_, 42, 430, 340, 24);
            place(instruction_, 42, 459, width - 84, 24);
        }
        place(simDirections_, 42, 460, width - 84, 22);
        place(simArming_, 42, 493, width - 84, 22);

        constexpr int footer = 552;
        place(folder_, width - 650, footer, 148, 31);
        place(select_, width - 492, footer, 180, 31);
        place(reload_, width - 302, footer, 106, 31);
        place(save_, width - 186, footer, 162, 31);
        if (width >= 820 || simulation_) {
            place(profileName_, 24, footer + 6, width - 684, 22);
            place(status_, 24, footer + 39, width - 48, 22);
        } else {
            place(profileName_, 24, footer + 39, 184, 22);
            place(status_, 220, footer + 39, width - 244, 22);
        }

        // Move the controls as a group and repaint the old and new positions.
        // Copying old client pixels during a resize can leave fragments of owner-drawn buttons.
        constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
        auto batch = BeginDeferWindowPos(static_cast<int>(placements.size()));
        for (const auto& position : placements) {
            if (!batch)
                break;
            batch = DeferWindowPos(batch, position.control, nullptr, position.x, position.y,
                                   position.width, position.height, flags);
        }
        if (!batch || !EndDeferWindowPos(batch)) {
            for (const auto& position : placements) {
                SetWindowPos(position.control, nullptr, position.x, position.y,
                             position.width, position.height, flags);
            }
        }
        redrawControls();
    }

    void paint()
    {
        PAINTSTRUCT paintData{};
        const auto dc = BeginPaint(window_, &paintData);
        RECT client{};
        GetClientRect(window_, &client);
        const auto& region = paintData.rcPaint;
        const int width = region.right - region.left;
        const int height = region.bottom - region.top;
        if (width > 0 && height > 0) {
            const auto left = contentBounds(client.right).first;
            const auto illustration = guideIllustrationBounds(left);
            const bool illustrationOnly = calibrating_ && step_ < stepCount_ && EqualRect(&region, &illustration);
            const auto paintRegion = [&](HDC target) {
                if (illustrationOnly)
                    paintGuideIllustration(target, left);
                else
                    paintScene(target, client);
            };
            const auto buffer = CreateCompatibleDC(dc);
            const auto bitmap = buffer ? CreateCompatibleBitmap(dc, width, height) : nullptr;
            if (bitmap) {
                const auto previous = SelectObject(buffer, bitmap);
                SetViewportOrgEx(buffer, -region.left, -region.top, nullptr);
                paintRegion(buffer);
                SetViewportOrgEx(buffer, 0, 0, nullptr);
                BitBlt(dc, region.left, region.top, width, height, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, previous);
                DeleteObject(bitmap);
            } else {
                paintRegion(dc);
            }
            if (buffer)
                DeleteDC(buffer);
        }
        EndPaint(window_, &paintData);
    }

    void paintScene(HDC dc, const RECT& client)
    {
        const auto [left, width] = contentBounds(client.right);
        const bool highContrast = isHighContrast();
        fill(dc, client, highContrast ? GetSysColor(COLOR_WINDOW) : background);
        paintHeader(dc, dpi_, client.right, left + 24, px(left + width - 312 - languageButtonSpace), headerLogo_.get(),
                    fonts_[0], fonts_[2], tr(Text::mainBannerSubtitle));

        const auto drawCard = [&](int top, int bottom) {
            const RECT card{px(left + 22), px(top), px(left + width - 22), px(bottom)};
            cutPanel(dc, card, px(13), highContrast ? GetSysColor(COLOR_WINDOW) : panel,
                     highContrast ? GetSysColor(COLOR_WINDOWTEXT) : edge);
            fill(dc, {card.left, card.top + px(12), card.left + px(2), card.top + px(33)},
                 highContrast ? GetSysColor(COLOR_HIGHLIGHT) : cyan);
        };
        if (calibrating_) {
            drawCard(98, 535);
            const auto first = retry_ ? retry_->first : 0;
            const auto end = retry_ ? retry_->end : stepCount_;
            paintCalibrationProgress(dc, {px(left + 46), px(158), px(left + width - 46), px(169)}, dpi_,
                                     step_ - first, end - first);
            if (step_ < stepCount_)
                paintGuideIllustration(dc, left);
        } else {
            for (const auto& [top, bottom] : {std::pair{98, 222}, {235, 405}, {418, 535}})
                drawCard(top, bottom);
        }
        const LONG footerRule = px(542);
        fill(dc, {px(left + 22), footerRule, px(left + width - 22), footerRule + 1},
             highContrast ? GetSysColor(COLOR_WINDOWTEXT) : edge);
    }

    RECT guideIllustrationBounds(int left) const
    {
        return {px(left + 46), px(198), px(left + 254), px(400)};
    }

    void paintGuideIllustration(HDC dc, int left)
    {
        paintCalibrationPose(dc, guideIllustrationBounds(left), dpi_, steps_[step_].pose, guideMotion_, fonts_[0]);
        if (capture_) {
            const bool highContrast = isHighContrast();
            const auto track = captureProgressBounds(left);
            fill(dc, track, highContrast ? GetSysColor(COLOR_WINDOWTEXT) : edge);
            RECT progress = track;
            progress.right = progress.left + MulDiv(track.right - track.left,
                static_cast<int>(capture_->samplesCollected()), static_cast<int>(chrome_axis::CalibrationCapture::sampleCount));
            fill(dc, progress, highContrast ? GetSysColor(COLOR_HIGHLIGHT) : cyan);
        }
    }

    RECT captureProgressBounds(int left) const
    {
        return {px(left + 50), px(381), px(left + 250), px(387)};
    }

    void paintMeter(const DRAWITEMSTRUCT& item, std::size_t index)
    {
        const auto rect = item.rcItem;
        const bool highContrast = isHighContrast();
        fill(item.hDC, rect, highContrast ? GetSysColor(COLOR_BTNFACE) : field);
        const LONG origin = index == 0 ? (rect.right + rect.left) / 2 : rect.left;
        const auto width = static_cast<float>(rect.right - origin);
        const auto end = origin + static_cast<LONG>(std::lround(live_[index] * width));
        if (liveReady_[index] && end != origin)
            fill(item.hDC, {std::min(origin, end), rect.top, std::max(origin, end), rect.bottom},
                 highContrast ? GetSysColor(COLOR_HIGHLIGHT) : cyan);
        if (index == 0)
            fill(item.hDC, {origin, rect.top, origin + 1, rect.bottom},
                 highContrast ? GetSysColor(COLOR_WINDOWTEXT) : secondary);
    }

    void paintButton(const DRAWITEMSTRUCT& item)
    {
        const auto id = item.CtlID;
        if (id == frenchFlag || id == englishFlag) {
            const bool french = id == frenchFlag;
            const auto language = french ? Language::french : Language::english;
            paintLanguageButton(item, dpi_, french ? Flag::france : Flag::unitedKingdom,
                                selectedLanguage_ == language);
            return;
        }
        if (id != hardwareTab && id != simulationTab) {
            const bool primary = id == toggleSimulation || id == toggleUsb || id == saveProfile ||
                                 id == startCalibration || id == measure;
            const bool stop = (id == toggleSimulation && publisher_.enabled()) || (id == toggleUsb && usbEnabled_);
            const bool footer = id == saveProfile || id == reloadProfile || id == selectProfile || id == openProfileFolder;
            paintAction(item, fonts_[0], dpi_, footer ? background : panel, primary, stop);
            return;
        }

        const bool highContrast = isHighContrast();
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool current = (id == simulationTab && simulation_) || (id == hardwareTab && !simulation_);
        fill(item.hDC, item.rcItem, highContrast ? GetSysColor(COLOR_WINDOW) : background);
        const auto face = highContrast ? GetSysColor(COLOR_BTNFACE) : current ? yellow : background;
        const auto border = highContrast ? GetSysColor(COLOR_BTNTEXT) : disabled ? edge : yellow;
        const auto& box = item.rcItem;
        const LONG left = box.left, right = box.right - 1, top = box.top, bottom = box.bottom - 1;
        const LONG notch = px(11);
        const POINT shape[]{
            {left + notch, top}, {right, top}, {right, bottom}, {left, bottom}, {left, top + notch}
        };
        drawPolygon(item.hDC, shape, face, border);
        if (!highContrast && !current && !disabled) {
            const POINT corner[]{{left + 1, top + notch}, {left + notch, top + 1}, {left + notch, top + notch}};
            drawPolygon(item.hDC, corner, yellow, yellow);
        }
        const auto color = highContrast ? GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT)
            : disabled ? secondary : current ? background : yellow;
        const auto previous = SaveDC(item.hDC);
        SetTextColor(item.hDC, color);
        SetBkMode(item.hDC, TRANSPARENT);
        const auto font = SelectObject(item.hDC, fonts_[1]);
        wchar_t text[80]{};
        GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
        auto rect = item.rcItem;
        InflateRect(&rect, -px(8), 0);
        DrawTextW(item.hDC, text, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item.hDC, font);
        if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
            auto focus = item.rcItem;
            InflateRect(&focus, -px(4), -px(4));
            DrawFocusRect(item.hDC, &focus);
        }
        if (previous)
            RestoreDC(item.hDC, previous);
    }

    void paintCombo(const DRAWITEMSTRUCT& item)
    {
        paintComboItem(item, fonts_[0], dpi_);
    }

    void action(int command, int notification);
    void scanDevices();
    void fillDeviceCombo(std::size_t channel);
    void selectDevice(std::size_t channel);
    void readProfile();
    void readAccessories();
    void updateProfileView(const CalibrationScope& changed = allChannels);
    void rebuildPreview();
    bool ensureProfileDirectory(const std::filesystem::path& directory);
    bool confirmPersistentDirectory() const;
    bool confirmCurrentPreset();
    void chooseStoredProfile();
    void reassignStoredDevice();
    void transferProfiles(bool restoring);
    void updateLanguageIndicator(Language preference);
    void persist();
    void updateCalibrationView();
    void updateGuideVisibility();
    void updateAnimationPreference();
    void updateCalibrationMotion(bool foreground);
    void refreshUsbToggle();
    void changeUsbMode();
    void setMode(bool simulation);
    void applyMode();
    void updateSimulationView();
    void updateSimulationStatus();
    chrome_axis::ControlState previewSimulation();
    CalibrationScope assignedCalibrationScope() const;
    CalibrationScope availableCalibrationScope() const;
    std::wstring deviceName(const chrome_axis::DeviceInfo& selected) const;
    void selectCalibrationScope();
    void beginCalibration(const CalibrationScope& scope);
    void measureStep();
    void previousStep();
    void retryAxis(std::size_t channel);
    void cancelCurrentCalibration();
    void enterCalibrationReview();
    void rewindCalibration(std::size_t target);
    void finishCalibration();
    void finishCapture(const chrome_axis::AxisCapture& capture);
    void acceptAxis(const chrome_axis::AxisCandidate& candidate);
    void suspendHardware();
    void tick();
    void updateLiveControls(const chrome_axis::ControlState& state);
    void contextMenu(int x, int y);

    const chrome_axis::DeviceInfo* device(std::string_view id) const
    {
        const auto found = std::find_if(devices_.begin(), devices_.end(),
            [id](const chrome_axis::DeviceInfo& item) { return item.id == id; });
        return found == devices_.end() ? nullptr : &*found;
    }

    const std::string& deviceId(std::size_t channel) const
    {
        while (channel > 0 && deviceSelections_[channel] == sameAsPrevious)
            --channel;
        return deviceSelections_[channel];
    }
    bool canCalibrate(const CalibrationScope& scope) const
    {
        const auto available = availableCalibrationScope();
        for (std::size_t i = 0; i < scope.size(); ++i) {
            if (scope[i] && !available[i])
                return false;
        }
        return std::ranges::any_of(scope, std::identity{});
    }
    bool reviewingCalibration() const { return calibrating_ && step_ == stepCount_; }
    bool canToggleUsb() const { return usbEnabled_ || (!calibrating_ && (saved_ || accessoriesSaved_)); }
    bool clearAnalogProfile() const
    {
        return saved_ && deviceId(0).empty() && deviceId(1).empty() && deviceId(2).empty();
    }
    bool canSaveProfile() const
    {
        return !simulation_ && !calibrating_ && dirty_ && (previewProfile_ || clearAnalogProfile());
    }
    const wchar_t* usbActionLabel() const
    {
        if (!usbEnabled_)
            return tr(Text::mainEnableUsb);
        return installed_ ? tr(Text::mainDisableUsb) : tr(Text::mainClearLocalUsb);
    }
    const std::filesystem::path& activeDirectory() const { return preset_.directory; }

    std::filesystem::path directory_;
    std::filesystem::path gameRoot_;
    std::filesystem::path path_;
    chrome_axis::ProfilePreset preset_;
    HWND window_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    std::array<HFONT, 3> fonts_{};
    Icon headerLogo_;
    HBRUSH backgroundBrush_{}, panelBrush_{}, fieldBrush_{};
    HMENU profileMenu_{}, deviceMenu_{}, languageMenu_{}, helpMenu_{};
    HWND devicesTitle_{}, axesTitle_{}, calibrationTitle_{};
    HWND accessories_{}, diagnostics_{}, feedback_{}, refresh_{}, hardware_{}, instruction_{}, start_{};
    HWND calibrationScopeButton_{}, axisChoice_{}, measure_{}, cancel_{}, previous_{};
    HWND guideStep_{}, guideAction_{}, guideDevice_{}, guideNotice_{};
    HWND profileName_{}, status_{}, folder_{}, select_{}, reload_{}, save_{};
    HWND hardwareTab_{}, simulationTab_{}, usbToggle_{}, simToggle_{}, simState_{}, simHint_{}, simDirections_{}, simArming_{};
    HWND frenchFlag_{}, englishFlag_{};
    Language selectedLanguage_{chrome_axis::interfaceLanguage()};
    std::array<HWND, 4> headings_{};
    std::array<HWND, 3> deviceLabels_{}, deviceCombos_{};
    std::array<HWND, 3> names_{}, bindings_{}, zones_{}, meters_{}, values_{};
    std::array<HWND, 3> simKeys_{};
    std::array<HWND, 3> retryAxes_{};
    std::unique_ptr<chrome_axis::DirectInput> input_;
    std::vector<chrome_axis::DeviceInfo> devices_;
    std::array<std::vector<std::string>, 3> deviceChoices_;
    std::array<std::string, 3> deviceSelections_;
    std::array<std::optional<chrome_axis::AxisCalibration>, 3> draft_{};
    std::optional<chrome_axis::Profile> previewProfile_;
    std::optional<chrome_axis::AxisCalibration> savedSteering_;
    std::optional<chrome_axis::ControlReader> preview_;
    std::array<float, 3> live_{};
    std::array<bool, 3> liveReady_{};
    std::array<int, 3> meterSpans_{};
    bool loadingControls_{}, saved_{}, dirty_{};
    bool accessoriesSaved_{};
    bool temporaryDirectory_{};
    bool deviceRefreshPending_{};
    bool usbEnabled_{};
    bool installed_{};
    std::wstring profileMessage_;

    bool calibrating_{}, guideVisible_{}, reviewVisible_{};
    bool guideMotionEnabled_{true};
    float guideMotion_{1.0f};
    std::chrono::steady_clock::time_point guideMotionStart_{};
    CalibrationScope calibrationScope_{};
    std::array<Step, calibrationSteps.size()> steps_{};
    std::size_t step_{}, stepCount_{};
    std::optional<chrome_axis::CalibrationCapture> capture_;
    unsigned int captureTicks_{};
    std::array<chrome_axis::AxisCapture, calibrationSteps.size()> positions_{};
    std::vector<chrome_axis::AxisCandidate> candidates_;
    chrome_axis::Profile measured_{};
    std::optional<AxisRetry> retry_;
    std::optional<chrome_axis::ControlReader> reviewPreview_;
    std::wstring calibrationMessage_;
    bool simulation_{};
    bool publisherFailed_{};
    chrome_axis::SimulationPublisher publisher_;
    chrome_axis::DiagnosticReader simulationDiagnostics_;
    std::uint64_t simulationRequestedAt_{};
    std::uint64_t nextSimulationStatus_{};
    chrome_axis::Simulator simulator_;
    std::chrono::steady_clock::time_point previewTime_{};
    std::chrono::steady_clock::time_point nextUsbRefresh_{};
};

void App::action(int command, int notification)
{
    if (command >= deviceComboFirst && command <= brakeCombo) {
        if (notification == CBN_SELCHANGE)
            selectDevice(static_cast<std::size_t>(command - deviceComboFirst));
        return;
    }
    if (command >= deadzoneFirst && command < deadzoneFirst + static_cast<int>(commands.size())) {
        if (notification == EN_CHANGE && !loadingControls_) {
            const auto index = static_cast<std::size_t>(command - deadzoneFirst);
            if (const auto value = readDeadzone(zones_[index]); draft_[index] && value)
                draft_[index]->deadzone = *value;
            dirty_ = true;
            profileMessage_.clear();
            rebuildPreview();
        }
        return;
    }
    if (command >= retryAxisFirst && command < retryAxisFirst + static_cast<int>(commands.size())) {
        if (notification == BN_CLICKED)
            retryAxis(static_cast<std::size_t>(command - retryAxisFirst));
        return;
    }
    switch (command) {
    case hardwareTab:
        setMode(false);
        break;
    case simulationTab:
        setMode(true);
        break;
    case toggleSimulation:
        if (simulation_) {
            publisherFailed_ = false;
            if (publisher_.enabled()) {
                publisher_.disable();
            } else if (!deployedProfileDirectory(directory_)) {
                MessageBoxW(window_, pluginInstallAdvice(directory_.parent_path(), Text::mainDeployAndOpen),
                            title, MB_OK | MB_ICONWARNING);
            } else {
                publisherFailed_ = !publisher_.enable();
                if (!publisherFailed_)
                    simulationRequestedAt_ = GetTickCount64();
            }
            updateSimulationView();
        }
        break;
    case toggleUsb:
        if (!simulation_)
            changeUsbMode();
        break;
    case refreshDevices:
        if (!simulation_)
            scanDevices();
        break;
    case reassignDevice:
        reassignStoredDevice();
        break;
    case configureAccessories:
        if (!simulation_ && !calibrating_ && confirmCurrentPreset()) {
            if (chrome_axis::editAccessories(window_, *input_, activeDirectory())) {
                readAccessories();
                profileMessage_.clear();
                rebuildPreview();
            }
            scanDevices();
        }
        break;
    case configureFeedback:
        if (!simulation_ && !calibrating_ && confirmCurrentPreset()) {
            const auto wheel = savedSteering_ ? device(savedSteering_->deviceId) : nullptr;
            const auto axis = savedSteering_ ? savedSteering_->axis : chrome_axis::axisCount;
            chrome_axis::editForceFeedback(window_, activeDirectory(), wheel, axis);
            scanDevices();
        }
        break;
    case showDiagnostics:
        if (!calibrating_)
            chrome_axis::showDiagnostics(window_, directory_, devices_);
        break;
    case startCalibration:
        beginCalibration(assignedCalibrationScope());
        break;
    case chooseCalibrationScope:
        selectCalibrationScope();
        break;
    case measure:
        measureStep();
        break;
    case previousCalibration:
        previousStep();
        break;
    case cancelCalibration:
        cancelCurrentCalibration();
        break;
    case saveProfile:
        persist();
        break;
    case reloadProfile:
        if (!simulation_ && !calibrating_ && (!dirty_ || MessageBoxW(window_,
                tr(Text::mainDiscardOnReload),
                title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)) {
            readProfile();
            scanDevices();
        }
        break;
    case selectProfile:
        chooseStoredProfile();
        break;
    case backupProfiles:
    case restoreProfiles:
        transferProfiles(command == restoreProfiles);
        break;
    case openProfileFolder:
        if (ensureProfileDirectory(directory_) && reinterpret_cast<INT_PTR>(
                ShellExecuteW(window_, L"open", directory_.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            MessageBoxW(window_, tr(Text::mainOpenFolderFailed), title, MB_OK | MB_ICONERROR);
        }
        break;
    case closeApp:
        SendMessageW(window_, WM_CLOSE, 0, 0);
        break;
    case automaticLanguage:
    case frenchLanguage:
    case englishLanguage:
    case frenchFlag:
    case englishFlag: {
        const int languageCommand = command == frenchFlag ? frenchLanguage : command == englishFlag ? englishLanguage : command;
        const auto option = std::ranges::find(languageOptions, languageCommand, &LanguageOption::command);
        const bool saved = deployedProfileDirectory(directory_)
            ? ensureProfileDirectory(directory_) &&
                chrome_axis::writeProfileLanguage(directory_, chrome_axis::languageCode(option->language))
            : chrome_axis::saveLanguageChoice(option->language);
        if (!saved) {
            MessageBoxW(window_, tr(Text::mainLanguageSaveFailed), title, MB_OK | MB_ICONERROR);
            break;
        }
        updateLanguageIndicator(option->language);
        if (selectedLanguage_ != chrome_axis::interfaceLanguage())
            MessageBoxW(window_, tr(Text::mainLanguageSaved), title, MB_OK | MB_ICONINFORMATION);
        break;
    }
    case openModInfo:
        if (reinterpret_cast<INT_PTR>(
                ShellExecuteW(window_, L"open", nexusModUrl, nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            MessageBoxW(window_, tr(Text::mainOpenModInfoFailed), title, MB_OK | MB_ICONERROR);
        }
        break;
    case aboutApp: {
        const auto description = std::wstring(title) + L" · " CHROME_AXIS_VERSION_WIDE L"\n"
            + tr(Text::mainAboutBody);
        MSGBOXPARAMSW about{sizeof(about)};
        about.hwndOwner = window_;
        about.hInstance = GetModuleHandleW(nullptr);
        about.lpszText = description.c_str();
        about.lpszCaption = title;
        about.dwStyle = MB_OK | MB_USERICON;
        about.lpszIcon = MAKEINTRESOURCEW(logoResource);
        MessageBoxIndirectW(&about);
        break;
    }
    }
}

void App::scanDevices()
{
    deviceRefreshPending_ = false;
    installed_ = chrome_axis::isInstalledPluginDirectory(directory_.parent_path());
    temporaryDirectory_ = chrome_axis::isTemporaryProfileDirectory(directory_);
    if (!input_->ready())
        input_ = std::make_unique<chrome_axis::DirectInput>(window_);
    devices_ = input_->devices();
    bool disconnected = false;
    for (std::size_t i = 0; i < deviceCombos_.size(); ++i) {
        fillDeviceCombo(i);
        const auto& id = deviceId(i);
        disconnected = disconnected || (!id.empty() && !device(id));
    }

    std::wstring hardwareStatus;
    std::wstring_view advice;
    if (!input_->ready()) {
        hardwareStatus = tr(Text::mainDirectInputUnavailable);
        advice = tr(Text::mainTryRefresh);
    } else if (devices_.empty()) {
        hardwareStatus = tr(Text::mainNoDevices);
        advice = tr(Text::mainConnectAndRefresh);
    } else if (disconnected) {
        hardwareStatus = tr(Text::mainSelectedDisconnected);
        advice = tr(Text::mainSettingsKept);
    } else {
        hardwareStatus = std::to_wstring(devices_.size()) +
            (devices_.size() == 1 ? tr(Text::mainDeviceDetected) : tr(Text::mainDevicesDetected));
    }
    if (temporaryDirectory_) {
        hardwareStatus = tr(Text::mainTemporaryCopy);
        advice = {};
    } else if (!installed_)
        advice = pluginInstallAdvice(directory_.parent_path(), Text::mainOpenFromGameForUsb);
    if (!advice.empty()) {
        hardwareStatus += L". ";
        hardwareStatus += advice;
    }
    setText(hardware_, hardwareStatus);
    if (calibrating_ && !reviewingCalibration() && !retry_ && !canCalibrate(calibrationScope_)) {
        calibrating_ = false;
        capture_.reset();
        candidates_.clear();
        calibrationMessage_ = tr(Text::mainReconnectBeforeCalibration);
    }
    updateCalibrationView();
}

void App::fillDeviceCombo(std::size_t channel)
{
    const HWND control = deviceCombos_[channel];
    auto& choices = deviceChoices_[channel];
    const auto& selection = deviceSelections_[channel];
    if (SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0))
        SendMessageW(control, CB_SHOWDROPDOWN, FALSE, 0);
    SendMessageW(control, CB_RESETCONTENT, 0, 0);
    choices = {{}};
    addChoice(control, tr(Text::mainGameControls));
    if (channel > 0) {
        choices.emplace_back(sameAsPrevious);
        addChoice(control, tr(sharedDeviceLabels[channel - 1]));
    }
    for (const auto& item : devices_) {
        if (!hasAxes(item))
            continue;
        choices.push_back(item.id);
        addChoice(control, deviceName(item));
    }
    auto found = std::find(choices.begin(), choices.end(), selection);
    if (found == choices.end()) {
        choices.push_back(selection);
        addChoice(control, tr(Text::mainProfileDeviceDisconnected));
        found = std::prev(choices.end());
    }
    SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(found - choices.begin()), 0);
}

std::wstring App::deviceName(const chrome_axis::DeviceInfo& selected) const
{
    const auto duplicates = std::count_if(devices_.begin(), devices_.end(), [&](const auto& other) {
        return hasAxes(other) && selected.name == other.name;
    });
    auto name = wide(selected.name);
    if (duplicates > 1)
        name += L" [" + wide(selected.id.substr(29, 8)) + L"]";
    return name;
}

void App::selectDevice(std::size_t channel)
{
    if (calibrating_)
        return;
    const auto& choices = deviceChoices_[channel];
    const auto index = SendMessageW(deviceCombos_[channel], CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<std::size_t>(index) >= choices.size())
        return;
    deviceSelections_[channel] = choices[static_cast<std::size_t>(index)];
    for (std::size_t i = 0; i < draft_.size(); ++i) {
        if (draft_[i] && draft_[i]->deviceId != deviceId(i)) {
            draft_[i].reset();
            dirty_ = true;
        }
    }
    profileMessage_.clear();
    calibrationMessage_.clear();
    updateProfileView();
    updateCalibrationView();
}

void App::readAccessories()
{
    std::string error;
    const auto bindings = activeDirectory().empty() ? std::nullopt
        : chrome_axis::loadAccessoryProfile(activeDirectory() / chrome_axis::accessoryFilename, error);
    accessoriesSaved_ = bindings && !bindings->empty();
}

void App::readProfile()
{
    preset_ = chrome_axis::activePreset(directory_);
    path_ = activeDirectory().empty() ? std::filesystem::path{} : activeDirectory() / chrome_axis::profileFilename;
    std::string error;
    const auto profile = path_.empty() ? std::nullopt : chrome_axis::loadProfile(path_, error);
    savedSteering_ = profile && !profile->steering.deviceId.empty() ? std::optional{profile->steering} : std::nullopt;
    saved_ = profile.has_value();
    dirty_ = false;
    profileMessage_.clear();
    calibrationMessage_.clear();
    draft_ = {};
    deviceSelections_ = {};
    if (profile) {
        const std::array axes{&profile->steering, &profile->throttle, &profile->brake};
        for (std::size_t i = 0; i < axes.size(); ++i) {
            if (!axes[i]->deviceId.empty()) {
                draft_[i] = *axes[i];
                deviceSelections_[i] = i > 0 && axes[i]->deviceId == axes[i - 1]->deviceId
                    ? sameAsPrevious : std::string_view(axes[i]->deviceId);
            }
        }
    } else {
        deviceSelections_[2] = sameAsPrevious;
        std::error_code ec;
        if (path_.empty()) {
            profileMessage_ = tr(Text::mainActiveProfileMissing);
        } else if (std::filesystem::exists(path_, ec) || ec) {
            profileMessage_ = tr(Text::mainInvalidCalibrationFile);
        }
    }
    setText(profileName_, activeDirectory().empty() ? tr(Text::mainProfileUnavailable)
        : preset_.name.empty() ? tr(Text::mainDefaultProfile) : tr(Text::mainProfilePrefix) + preset_.name);
    readAccessories();
    updateProfileView();
}

void App::updateProfileView(const CalibrationScope& changed)
{
    loadingControls_ = true;
    for (std::size_t i = 0; i < draft_.size(); ++i) {
        if (!changed[i])
            continue;
        setText(bindings_[i], draft_[i] ? wide(chrome_axis::axisName(draft_[i]->axis)) : tr(Text::mainNeedsCalibration));
        setText(zones_[i], draft_[i] ? formatDeadzone(draft_[i]->deadzone) : L"");
        EnableWindow(zones_[i], draft_[i].has_value() && !calibrating_);
    }
    loadingControls_ = false;
    rebuildPreview();
}

void App::rebuildPreview()
{
    previewProfile_.reset();
    preview_.reset();
    bool zonesValid = true;
    bool assignmentsCalibrated = true;
    for (std::size_t i = 0; i < draft_.size(); ++i) {
        if (draft_[i] && !readDeadzone(zones_[i]))
            zonesValid = false;
        if (!deviceId(i).empty() && !draft_[i])
            assignmentsCalibrated = false;
    }
    const bool pedalsCompatible = !draft_[1] || !draft_[2] ||
        chrome_axis::compatibleAxisPair(*draft_[1], *draft_[2]);
    if (zonesValid && pedalsCompatible && assignmentsCalibrated) {
        const chrome_axis::Profile profile{draft_[0].value_or(chrome_axis::AxisCalibration{}),
            draft_[1].value_or(chrome_axis::AxisCalibration{}), draft_[2].value_or(chrome_axis::AxisCalibration{})};
        std::string error;
        if (chrome_axis::validateProfile(profile, error)) {
            previewProfile_ = profile;
            preview_.emplace(profile);
        }
    }
    const bool canSave = canSaveProfile();
    EnableWindow(save_, canSave);
    EnableMenuItem(profileMenu_, saveProfile, MF_BYCOMMAND | (canSave ? MF_ENABLED : MF_GRAYED));
    setText(status_, !profileMessage_.empty() ? profileMessage_
        : !zonesValid ? tr(Text::mainDeadzoneRange)
        : !pedalsCompatible ? tr(Text::mainIncompatibleDeadzones)
        : dirty_ && clearAnalogProfile() ? tr(Text::mainSaveToClearCalibration)
        : dirty_ ? (canSave ? tr(Text::mainSaveToApplySettings) : tr(Text::mainIncompleteCalibration))
        : saved_ ? tr(Text::mainCalibrationSaved)
        : accessoriesSaved_ ? tr(Text::mainAccessoriesSaved)
        : tr(Text::mainNoProfile));
}

bool App::ensureProfileDirectory(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (!error)
        return true;
    MessageBoxW(window_, tr(Text::mainCreateProfileFolderFailed),
        title, MB_OK | MB_ICONERROR);
    return false;
}

bool App::confirmPersistentDirectory() const
{
    if (!temporaryDirectory_)
        return true;
    MessageBoxW(window_, tr(Text::mainExtractBeforeSaving),
        title, MB_OK | MB_ICONWARNING);
    return false;
}

bool App::confirmCurrentPreset()
{
    const auto current = chrome_axis::activePreset(directory_);
    if (!current.directory.empty() && current.directory == activeDirectory())
        return true;

    MessageBoxW(window_, current.directory.empty() ? tr(Text::mainSelectAvailableProfile)
        : tr(Text::mainActiveProfileChanged), title, MB_OK | MB_ICONWARNING);
    return false;
}

void App::chooseStoredProfile()
{
    if (simulation_ || calibrating_)
        return;

    const auto names = chrome_axis::listPresets(directory_);
    const auto choice = chrome_axis::chooseProfile(window_, names, preset_.name);
    if (!choice)
        return;
    if (!choice->copy && !activeDirectory().empty() && choice->name == preset_.name
        && chrome_axis::activePreset(directory_).directory == activeDirectory())
        return;

    if (dirty_) {
        const auto response = MessageBoxW(window_,
            tr(Text::mainSaveBeforeSwitch),
            title, MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (response == IDYES) {
            if (!canSaveProfile()) {
                MessageBoxW(window_, tr(Text::mainFinishOrDiscard),
                            title, MB_OK | MB_ICONWARNING);
                return;
            }
            persist();
            if (dirty_)
                return;
        } else if (response != IDNO) {
            return;
        }
    }

    if (choice->copy && (!confirmPersistentDirectory() || !confirmCurrentPreset()))
        return;

    std::string error;
    const bool selected = choice->copy ? chrome_axis::createPreset(directory_, choice->name, error)
        : chrome_axis::selectPreset(directory_, choice->name, error);
    if (!selected) {
        MessageBoxW(window_, chrome_axis::localizedError(error).c_str(), title, MB_OK | MB_ICONWARNING);
        return;
    }
    readProfile();
    scanDevices();
}

void App::updateLanguageIndicator(Language preference)
{
    const auto option = std::ranges::find(languageOptions, preference, &LanguageOption::language);
    CheckMenuRadioItem(languageMenu_, automaticLanguage, englishLanguage, option->command, MF_BYCOMMAND);
    selectedLanguage_ = chrome_axis::resolveLanguage(preference);
    InvalidateRect(frenchFlag_, nullptr, FALSE);
    InvalidateRect(englishFlag_, nullptr, FALSE);
}

void App::reassignStoredDevice()
{
    if (simulation_ || calibrating_ || !confirmPersistentDirectory() || !confirmCurrentPreset())
        return;
    if (dirty_) {
        if (MessageBoxW(window_, tr(Text::mainReassignSaveDraft), title,
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        if (!canSaveProfile()) {
            MessageBoxW(window_, tr(Text::mainFinishOrDiscard), title, MB_OK | MB_ICONWARNING);
            return;
        }
        persist();
        if (dirty_)
            return;
    }

    const auto expected = preset_;
    const auto readSavedBindings = [&](chrome_axis::Profile& profile, chrome_axis::AccessoryProfile& accessories) {
        const auto exists = [](const auto& file) {
            std::error_code error;
            return std::filesystem::exists(file, error) || error;
        };
        std::string error;
        const auto axesFile = expected.directory / chrome_axis::profileFilename;
        if (exists(axesFile)) {
            const auto loaded = chrome_axis::loadProfile(axesFile, error);
            if (!loaded)
                return false;
            profile = *loaded;
        }
        const auto accessoryFile = expected.directory / chrome_axis::accessoryFilename;
        if (exists(accessoryFile)) {
            auto loaded = chrome_axis::loadAccessoryProfile(accessoryFile, error);
            if (!loaded)
                return false;
            accessories = std::move(*loaded);
        }
        return true;
    };
    chrome_axis::Profile profile;
    chrome_axis::AccessoryProfile accessories;
    if (!readSavedBindings(profile, accessories)) {
        MessageBoxW(window_, tr(Text::mainReassignInvalidProfile), title, MB_OK | MB_ICONERROR);
        return;
    }
    scanDevices();
    const auto connected = devices_;
    const auto choice = chrome_axis::chooseDeviceReassignment(window_, profile, accessories, connected);
    if (!choice)
        return;

    const auto message = expected.name.empty() ? std::wstring(tr(Text::mainDefaultProfile))
        : tr(Text::mainProfilePrefix) + expected.name;
    const auto confirmed = message + L"\n\n" + tr(Text::mainReassignConfirm);
    if (MessageBoxW(window_, confirmed.c_str(), title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    if (!confirmCurrentPreset())
        return;
    scanDevices();
    chrome_axis::Profile currentProfile;
    chrome_axis::AccessoryProfile currentAccessories;
    const auto target = device(choice->target);
    if (!target || !readSavedBindings(currentProfile, currentAccessories) ||
        !chrome_axis::supportsReassignment(choice->source, *target, currentProfile, currentAccessories)) {
        MessageBoxW(window_, tr(Text::mainReassignChanged), title, MB_OK | MB_ICONWARNING);
        return;
    }

    using Error = chrome_axis::DeviceReplacementError;
    Error error{};
    std::filesystem::path recovery;
    if (!chrome_axis::replacePresetDevice(directory_, expected, choice->source, choice->target, error, &recovery)) {
        auto reason = Text::mainReassignFailed;
        switch (error) {
        case Error::invalidSelection:
        case Error::deviceNotMapped: reason = Text::mainReassignChanged; break;
        case Error::invalidProfile: reason = Text::mainReassignInvalidProfile; break;
        case Error::conflict: reason = Text::mainReassignConflict; break;
        case Error::recoveryRequired: reason = Text::mainReassignRecovery; break;
        default: break;
        }
        auto failure = std::wstring(tr(reason));
        if (error == Error::recoveryRequired)
            failure += L"\n\n" + recovery.wstring();
        MessageBoxW(window_, failure.c_str(), title, MB_OK | MB_ICONERROR);
        return;
    }
    readProfile();
    profileMessage_ = tr(Text::mainReassignSaved);
    scanDevices();
    rebuildPreview();
}

void App::transferProfiles(bool restoring)
{
    if (calibrating_ || simulation_)
        return;

    const auto confirmDeployment = [&] {
        if (deployedProfileDirectory(directory_))
            return true;
        MessageBoxW(window_, tr(Text::mainDeployAndOpen), title, MB_OK | MB_ICONWARNING);
        return false;
    };
    if (!confirmDeployment())
        return;

    const auto backups = directory_ / chrome_axis::profileBackupsFolder;
    std::error_code directoryError;
    const auto initial = std::filesystem::is_directory(backups, directoryError) ? backups : directory_;
    std::filesystem::path chosen;
    const auto result = chrome_axis::chooseProfileFolder(window_,
        tr(restoring ? Text::mainRestorePickFolder : Text::mainBackupPickFolder), initial, chosen);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    if (FAILED(result)) {
        MessageBoxW(window_, tr(Text::mainPickFolderFailed), title, MB_OK | MB_ICONERROR);
        return;
    }
    if (!confirmDeployment())
        return;

    if (restoring) {
        auto confirmation = std::wstring(tr(Text::mainRestoreConfirm)) + L"\n\n" + directory_.wstring();
        if (dirty_)
            confirmation += L"\n\n" + std::wstring(tr(Text::mainRestoreDiscardDraft));
        if (MessageBoxW(window_, confirmation.c_str(), title,
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
    } else if (dirty_) {
        const auto answer = MessageBoxW(window_, tr(Text::mainSaveBeforeBackup), title,
                                       MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (answer == IDYES) {
            if (!canSaveProfile()) {
                MessageBoxW(window_, tr(Text::mainFinishOrDiscard), title, MB_OK | MB_ICONWARNING);
                return;
            }
            persist();
            if (dirty_)
                return;
        } else if (answer != IDNO) {
            return;
        }
    }
    if (!confirmDeployment())
        return;

    using Error = chrome_axis::ProfileBackupError;
    Error error{};
    const auto fallback = chrome_axis::languageCode(chrome_axis::profileLanguageChoice(directory_));
    std::error_code sameDirectoryError;
    const bool choseActiveFolder = !restoring && std::filesystem::equivalent(chosen, directory_, sameDirectoryError);
    const auto path = restoring ? chosen : (choseActiveFolder ? backups : chosen) / backupFolderName();
    std::filesystem::path recovery;
    const bool complete = restoring ? chrome_axis::restoreProfileBackup(directory_, path, error, fallback, &recovery)
                                    : chrome_axis::createProfileBackup(directory_, path, error, fallback);
    if (!complete) {
        auto reason = Text::mainProfileBackupIO;
        switch (error) {
        case Error::invalidLocation: reason = Text::mainProfileBackupInvalidLocation; break;
        case Error::invalidBackup: reason = Text::mainProfileBackupInvalid; break;
        case Error::destinationExists: reason = Text::mainProfileBackupExists; break;
        case Error::recoveryRequired: reason = Text::mainProfileBackupRecoveryRequired; break;
        default: break;
        }
        auto message = std::wstring(tr(reason));
        if (error == Error::recoveryRequired)
            message += L"\n\n" + recovery.wstring();
        MessageBoxW(window_, message.c_str(), title, MB_OK | MB_ICONERROR);
        return;
    }
    auto message = std::wstring(tr(restoring ? Text::mainRestoreComplete : Text::mainBackupComplete));
    if (restoring) {
        readProfile();
        scanDevices();
        updateLanguageIndicator(chrome_axis::profileLanguageChoice(directory_));
        message += L"\n\n" + std::wstring(tr(Text::mainRestoreRecovery)) + L"\n" + recovery.wstring();
        if (selectedLanguage_ != chrome_axis::interfaceLanguage())
            message += L"\n\n" + std::wstring(tr(Text::mainLanguageSaved));
    } else {
        message += L"\n\n" + path.wstring();
    }
    MessageBoxW(window_, message.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

void App::persist()
{
    if (!canSaveProfile() || !confirmPersistentDirectory() || !confirmCurrentPreset())
        return;

    const auto nextSteering = previewProfile_ && !previewProfile_->steering.deviceId.empty()
        ? std::optional{previewProfile_->steering} : std::nullopt;
    const bool changedActuator = savedSteering_.has_value() != nextSteering.has_value()
        || (savedSteering_ && nextSteering && (savedSteering_->deviceId != nextSteering->deviceId
            || savedSteering_->axis != nextSteering->axis));
    bool resetFeedback = false;
    if (changedActuator) {
        const auto settingsPath = activeDirectory() / chrome_axis::feedbackFilename;
        std::string error;
        auto settings = chrome_axis::loadFeedbackSettings(settingsPath, error);
        if (settings && settings->enabled) {
            settings->enabled = false;
            if (!chrome_axis::saveFeedbackSettings(settingsPath, *settings, error)) {
                MessageBoxW(window_, tr(Text::mainDisableOldFeedbackFailed),
                    title, MB_OK | MB_ICONERROR);
                return;
            }
            resetFeedback = true;
        }
    }

    bool success = false;
    if (previewProfile_) {
        if (!ensureProfileDirectory(activeDirectory()))
            return;
        std::string error;
        success = chrome_axis::saveProfile(path_, *previewProfile_, error);
    } else {
        success = DeleteFileW(path_.c_str()) != FALSE;
        if (!success) {
            const auto error = GetLastError();
            success = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
    }
    if (!success) {
        MessageBoxW(window_, resetFeedback
            ? tr(Text::mainSaveFailedFeedbackDisabled)
            : tr(Text::mainSaveFailedCheckFolder),
            title, MB_OK | MB_ICONERROR);
        return;
    }
    dirty_ = false;
    saved_ = previewProfile_.has_value();
    savedSteering_ = nextSteering;
    refreshUsbToggle();
    profileMessage_ = !saved_ ? tr(Text::mainAxesUseGameControls)
        : resetFeedback ? tr(Text::mainSavedCheckFeedback)
        : !installed_ ? tr(Text::mainSavedLocally)
        : usbEnabled_ ? tr(Text::mainSavedUsbRequested)
        : tr(Text::mainSavedEnableUsb);
    rebuildPreview();
}

void App::refreshUsbToggle()
{
    usbEnabled_ = chrome_axis::usbEnabled(directory_);
    setText(usbToggle_, usbActionLabel());
    const bool enabled = canToggleUsb();
    if ((IsWindowEnabled(usbToggle_) != FALSE) != enabled)
        EnableWindow(usbToggle_, enabled);
    nextUsbRefresh_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

void App::changeUsbMode()
{
    readAccessories();
    refreshUsbToggle();
    if (!canToggleUsb())
        return;

    const bool enable = !usbEnabled_;
    std::string error;
    if (enable) {
        if (!confirmPersistentDirectory() || !confirmCurrentPreset())
            return;
        const auto pluginDirectory = directory_.parent_path();
        if (!chrome_axis::isInstalledPluginDirectory(pluginDirectory)) {
            const auto message = std::wstring(tr(Text::mainCurrentProfileFolder)) + L"\n\n" + directory_.wstring()
                + L"\n\n" + pluginInstallAdvice(pluginDirectory, Text::mainDeployAndOpen);
            MessageBoxW(window_, message.c_str(), title, MB_OK | MB_ICONWARNING);
            return;
        }
        if (dirty_ || (saved_ && !previewProfile_)) {
            const auto message = canSaveProfile()
                ? tr(Text::mainSaveOrReloadBeforeUsb)
                : tr(Text::mainCalibrateOrReloadBeforeUsb);
            MessageBoxW(window_, message, title, MB_OK | MB_ICONWARNING);
            return;
        }
        if (!accessoriesSaved_ && !chrome_axis::loadProfile(path_, error)) {
            MessageBoxW(window_, tr(Text::mainSaveCalibrationOrMapping),
                title, MB_OK | MB_ICONWARNING);
            return;
        }
    }
    if (!chrome_axis::setUsbEnabled(directory_, enable, error)) {
        MessageBoxW(window_, tr(Text::mainUsbStateFailed),
            title, MB_OK | MB_ICONERROR);
        return;
    }
    refreshUsbToggle();
    profileMessage_ = enable ? tr(Text::mainUsbRequested)
        : installed_ ? tr(Text::mainUsbDisabled) : tr(Text::mainLocalUsbCleared);
    setText(status_, profileMessage_);
}

void App::updateCalibrationView()
{
    if (simulation_) {
        updateSimulationView();
        return;
    }
    refreshUsbToggle();
    const auto available = availableCalibrationScope();
    const bool canStart = !calibrating_ && canCalibrate(assignedCalibrationScope());
    const bool canChooseScope = !calibrating_ && std::ranges::any_of(available, std::identity{});
    const bool choosing = !candidates_.empty();
    guideMotionStart_ = {};
    updateCalibrationMotion(GetForegroundWindow() == window_ && !IsIconic(window_));
    updateGuideVisibility();
    ShowWindow(axisChoice_, calibrating_ && choosing ? SW_SHOW : SW_HIDE);
    EnableWindow(start_, canStart);
    EnableWindow(calibrationScopeButton_, canChooseScope);
    const bool axisDeviceAvailable = std::ranges::any_of(devices_, hasAxes);
    for (std::size_t i = 0; i < deviceCombos_.size(); ++i)
        EnableWindow(deviceCombos_[i], !calibrating_ && (axisDeviceAvailable || !deviceSelections_[i].empty()));
    EnableWindow(accessories_, !calibrating_);
    EnableWindow(feedback_, !calibrating_);
    EnableWindow(diagnostics_, !calibrating_);
    EnableMenuItem(helpMenu_, showDiagnostics, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableWindow(reload_, !calibrating_);
    EnableWindow(select_, !calibrating_);
    EnableWindow(simulationTab_, !calibrating_);
    for (std::size_t i = 0; i < zones_.size(); ++i)
        EnableWindow(zones_[i], !calibrating_ && draft_[i].has_value());
    EnableMenuItem(deviceMenu_, startCalibration, MF_BYCOMMAND | (canStart ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(deviceMenu_, chooseCalibrationScope, MF_BYCOMMAND | (canChooseScope ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(deviceMenu_, refreshDevices, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(deviceMenu_, configureAccessories, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(deviceMenu_, reassignDevice, MF_BYCOMMAND |
        (calibrating_ || activeDirectory().empty() || temporaryDirectory_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(deviceMenu_, configureFeedback, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(profileMenu_, reloadProfile, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(profileMenu_, selectProfile, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(profileMenu_, backupProfiles, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(profileMenu_, restoreProfiles, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    const bool canSave = canSaveProfile();
    EnableWindow(save_, canSave);
    EnableMenuItem(profileMenu_, saveProfile, MF_BYCOMMAND | (canSave ? MF_ENABLED : MF_GRAYED));

    if (!calibrating_) {
        setText(calibrationTitle_, tr(Text::mainCalibration));
        setText(instruction_, !calibrationMessage_.empty() ? calibrationMessage_
            : canChooseScope ? tr(Text::mainChooseCalibrationScope)
            : !axisDeviceAvailable ? tr(Text::mainConnectForCalibration)
            : tr(Text::mainSelectAxisDevice));
        return;
    }

    setText(calibrationTitle_, tr(Text::guideTitle));
    const auto first = retry_ ? retry_->first : 0;
    const auto end = retry_ ? retry_->end : stepCount_;
    EnableWindow(previous_, step_ > first && !capture_);
    setText(cancel_, tr(retry_ ? Text::guideCancelRetry : Text::mainCancel));
    if (reviewingCalibration()) {
        setText(guideStep_, tr(Text::guideReview));
        setText(guideAction_, tr(Text::guideReviewTitle));
        setText(guideDevice_, L"");
        bool disconnected = false;
        for (std::size_t i = 0; i < retryAxes_.size(); ++i) {
            EnableWindow(retryAxes_[i], available[i]);
            disconnected = disconnected || (calibrationScope_[i] && !available[i]);
        }
        EnableWindow(previous_, !disconnected);
        setText(instruction_, tr(disconnected ? Text::guideReviewReconnect : Text::guideReviewInstruction));
        setText(measure_, tr(Text::guideFinish));
        EnableWindow(measure_, TRUE);
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    const auto& step = steps_[step_];
    setText(guideStep_, tr(Text::guideStepPrefix) + std::to_wstring(step_ - first + 1) + tr(Text::guideStepSeparator)
        + std::to_wstring(end - first));
    setText(guideAction_, tr(choosing ? Text::guideSelectAxis : step.heading));
    std::wstring deviceLine = tr(deviceLabels[step.channel]);
    if (const auto* selected = device(deviceId(step.channel)))
        deviceLine += L"  ·  " + deviceName(*selected);
    setText(guideDevice_, deviceLine);
    setText(instruction_, tr(choosing ? Text::mainChooseMatchingAxis : step.instruction));
    const bool disconnected = !available[step.channel];
    setText(guideNotice_, !calibrationMessage_.empty() ? calibrationMessage_
        : disconnected ? tr(Text::guideRetryReconnect)
        : choosing ? L"" : tr(step_ == first ? Text::guideCaptureHint : Text::guideNextCaptureHint));
    setText(measure_, capture_ ? tr(Text::mainMeasuring) : choosing ? tr(Text::mainChooseAxis) : tr(Text::guideCapture));
    EnableWindow(measure_, !capture_ && !disconnected);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::updateAnimationPreference()
{
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        guideMotionEnabled_ = enabled != FALSE;
}

void App::updateCalibrationMotion(bool foreground)
{
    if (!calibrating_ || step_ >= stepCount_)
        return;

    float motion = 1.0f;
    if (foreground && guideMotionEnabled_ && !capture_ && candidates_.empty() &&
        steps_[step_].pose != CalibrationPose::wheelCenter) {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (guideMotionStart_ == steady_clock::time_point{})
            guideMotionStart_ = now;
        constexpr milliseconds cycle{1800};
        constexpr milliseconds initialHold{180};
        constexpr milliseconds movement{670};
        const auto elapsed = duration_cast<milliseconds>(now - guideMotionStart_) % cycle;
        const float progress = std::clamp(static_cast<float>((elapsed - initialHold).count()) /
            static_cast<float>(movement.count()), 0.0f, 1.0f);
        motion = progress * progress * (3.0f - 2.0f * progress);
    } else {
        guideMotionStart_ = {};
    }

    if (motion != guideMotion_) {
        guideMotion_ = motion;
        RECT client{};
        GetClientRect(window_, &client);
        const auto illustration = guideIllustrationBounds(contentBounds(client.right).first);
        InvalidateRect(window_, &illustration, FALSE);
    }
}

void App::updateGuideVisibility()
{
    const auto show = [](HWND control, bool visible) {
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    };
    for (const auto control : {measure_, cancel_, previous_, guideStep_, guideAction_})
        show(control, calibrating_);
    const bool reviewing = reviewingCalibration();
    for (const auto control : {guideDevice_, guideNotice_})
        show(control, calibrating_ && !reviewing);
    if (guideVisible_ == calibrating_ && reviewVisible_ == reviewing)
        return;
    guideVisible_ = calibrating_;
    reviewVisible_ = reviewing;
    for (const auto control : {devicesTitle_, hardware_, usbToggle_, refresh_, accessories_, axesTitle_,
                              start_, calibrationScopeButton_, diagnostics_, feedback_})
        show(control, !calibrating_);
    for (const auto control : headings_)
        show(control, !calibrating_);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        for (const auto control : {deviceLabels_[i], deviceCombos_[i], bindings_[i], zones_[i]})
            show(control, !calibrating_);
        const bool reviewAxis = reviewing && calibrationScope_[i];
        for (const auto control : {names_[i], meters_[i], values_[i]})
            show(control, !calibrating_ || reviewAxis);
        show(retryAxes_[i], reviewAxis);
    }
    layout();
}

void App::setMode(bool simulation)
{
    if (simulation == simulation_ || calibrating_)
        return;
    publisher_.disable();
    publisherFailed_ = false;
    simulator_.reset();
    previewTime_ = {};
    suspendHardware();
    simulation_ = simulation;
    applyMode();
    layout();
    if (simulation_) {
        updateSimulationView();
    } else {
        rebuildPreview();
        scanDevices();
    }
    tick();
}

void App::applyMode()
{
    const auto setVisible = [](HWND control, bool visible) {
        constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
        SetWindowPos(control, nullptr, 0, 0, 0, 0, flags | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    };
    for (const auto control : {usbToggle_, refresh_, accessories_, diagnostics_, feedback_, hardware_, headings_[2],
                              instruction_, start_, calibrationScopeButton_, profileName_, folder_, select_, reload_, save_})
        setVisible(control, !simulation_);
    for (const auto control : {simToggle_, simState_, simHint_, simDirections_, simArming_})
        setVisible(control, simulation_);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        setVisible(deviceLabels_[i], !simulation_);
        setVisible(deviceCombos_[i], !simulation_);
        setVisible(bindings_[i], !simulation_);
        setVisible(zones_[i], !simulation_);
        setVisible(simKeys_[i], simulation_);
    }
    setText(devicesTitle_, simulation_ ? tr(Text::mainKeyboardSimulation) : tr(Text::mainDevicesHeading));
    setText(axesTitle_, simulation_ ? tr(Text::mainSimulatedSignals) : tr(Text::mainVehicleAxesHeading));
    setText(headings_[1], simulation_ ? tr(Text::mainKeys) : tr(Text::mainDetectedAxis));
    setText(headings_[3], simulation_ ? tr(Text::mainLivePreview) : tr(Text::mainLocalPreview));
    redrawControls();
}

void App::updateSimulationView()
{
    const bool active = publisher_.enabled();
    setText(simToggle_, active ? tr(Text::mainStopSimulation) : tr(Text::mainEnableInGame));
    InvalidateRect(simToggle_, nullptr, FALSE);
    setText(calibrationTitle_, tr(Text::mainTestInCyberpunk));
    updateSimulationStatus();
    EnableMenuItem(deviceMenu_, startCalibration, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, chooseCalibrationScope, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, refreshDevices, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, configureAccessories, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, reassignDevice, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, configureFeedback, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, saveProfile, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, reloadProfile, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, selectProfile, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, backupProfiles, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, restoreProfiles, MF_BYCOMMAND | MF_GRAYED);
}

void App::updateSimulationStatus()
{
    nextSimulationStatus_ = GetTickCount64() + 250;
    if (!publisher_.enabled()) {
        setText(simState_, publisherFailed_ ? tr(Text::mainSimulationFailed) : tr(Text::mainSimulationPreview));
        setText(status_, tr(Text::mainPreviewMode));
        return;
    }

    const auto report = simulationDiagnostics_.read();
    // The last driving snapshot survives Alt-Tab. A snapshot predating the
    // current activation cannot acknowledge this request.
    const bool confirmed = report.connection == chrome_axis::DiagnosticConnection::connected
        && report.lastDriveAt > simulationRequestedAt_
        && report.lastDrive.mode == chrome_axis::DiagnosticMode::simulation;
    setText(simState_, confirmed ? tr(Text::mainSimulationActive) : tr(Text::mainSimulationPending));
    setText(status_, tr(Text::mainKeepSimulationOpen));
}

chrome_axis::ControlState App::previewSimulation()
{
    const auto now = std::chrono::steady_clock::now();
    if (GetForegroundWindow() != window_ || IsIconic(window_)) {
        simulator_.reset();
        previewTime_ = {};
        return {};
    }
    const float seconds = previewTime_ == std::chrono::steady_clock::time_point{}
        ? 0.0f : std::chrono::duration<float>(now - previewTime_).count();
    previewTime_ = now;
    return {simulator_.update(chrome_axis::simulationKeys(), seconds), true, true, true};
}

CalibrationScope App::assignedCalibrationScope() const
{
    CalibrationScope scope{};
    for (std::size_t i = 0; i < scope.size(); ++i)
        scope[i] = !deviceId(i).empty();
    return scope;
}

CalibrationScope App::availableCalibrationScope() const
{
    CalibrationScope scope{};
    for (std::size_t i = 0; i < scope.size(); ++i) {
        const auto* selected = device(deviceId(i));
        scope[i] = selected && hasAxes(*selected);
    }
    return scope;
}

void App::selectCalibrationScope()
{
    if (simulation_ || calibrating_)
        return;
    const auto available = availableCalibrationScope();
    if (!std::ranges::any_of(available, std::identity{}))
        return;

    const HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    std::array<CalibrationScope, commands.size() * 2> options{};
    std::size_t optionCount{};
    const auto addOption = [&](const wchar_t* label, const CalibrationScope& scope) {
        if (AppendMenuW(menu, MF_STRING, optionCount + 1, label))
            options[optionCount++] = scope;
    };
    for (std::size_t i = 0; i < available.size(); ++i) {
        if (available[i]) {
            CalibrationScope scope{};
            scope[i] = true;
            addOption(tr(commands[i]), scope);
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (const auto& item : devices_) {
        CalibrationScope scope{};
        for (std::size_t i = 0; i < scope.size(); ++i)
            scope[i] = available[i] && deviceId(i) == item.id;
        if (std::ranges::any_of(scope, std::identity{})) {
            std::wstring label = tr(Text::mainCalibrationDevice);
            for (const auto ch : deviceName(item)) {
                if (ch == L'&')
                    label += ch;
                label += ch;
            }
            addOption(label.c_str(), scope);
        }
    }

    RECT bounds{};
    GetWindowRect(calibrationScopeButton_, &bounds);
    const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                         bounds.left, bounds.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
    if (selected > 0 && static_cast<std::size_t>(selected) <= optionCount)
        beginCalibration(options[static_cast<std::size_t>(selected) - 1]);
}

void App::beginCalibration(const CalibrationScope& scope)
{
    if (simulation_ || calibrating_ || !canCalibrate(scope))
        return;
    calibrationScope_ = scope;
    const auto planned = std::ranges::copy_if(calibrationSteps, steps_.begin(),
        [&scope](const Step& step) { return scope[step.channel]; });
    stepCount_ = static_cast<std::size_t>(planned.out - steps_.begin());
    measured_ = {};
    positions_ = {};
    retry_.reset();
    reviewPreview_.reset();
    const std::array axes{&measured_.steering, &measured_.throttle, &measured_.brake};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        if (!scope[i] && draft_[i])
            *axes[i] = *draft_[i];
    }
    calibrating_ = true;
    step_ = 0;
    candidates_.clear();
    calibrationMessage_.clear();
    updateCalibrationView();
    SetFocus(measure_);
}

void App::measureStep()
{
    if (!calibrating_ || capture_)
        return;
    if (step_ == stepCount_) {
        finishCalibration();
        return;
    }
    if (!availableCalibrationScope()[steps_[step_].channel])
        return;
    if (!candidates_.empty()) {
        const auto selected = SendMessageW(axisChoice_, CB_GETCURSEL, 0, 0);
        if (selected != CB_ERR && static_cast<std::size_t>(selected) < candidates_.size()) {
            const auto candidate = candidates_[static_cast<std::size_t>(selected)];
            acceptAxis(candidate);
        }
        return;
    }
    calibrationMessage_ = tr(Text::mainHoldSteady);
    capture_.emplace();
    captureTicks_ = 0;
    updateCalibrationView();
}

void App::previousStep()
{
    const auto first = retry_ ? retry_->first : 0;
    if (!calibrating_ || capture_ || step_ == first || (reviewingCalibration() && !canCalibrate(calibrationScope_)))
        return;

    reviewPreview_.reset();
    // From axis selection, return to the position just captured.
    rewindCalibration(candidates_.empty() ? step_ - 1 : step_);
    calibrationMessage_ = tr(Text::guideRetryPosition);
    updateCalibrationView();
    SetFocus(step_ > first ? previous_ : measure_);
}

void App::retryAxis(std::size_t channel)
{
    if (!reviewingCalibration() || !calibrationScope_[channel] || !availableCalibrationScope()[channel])
        return;

    const auto planned = std::span(steps_).first(stepCount_);
    const auto axisSteps = std::ranges::equal_range(planned, channel, {}, &Step::channel);
    const auto first = static_cast<std::size_t>(axisSteps.begin() - planned.begin());
    const auto end = static_cast<std::size_t>(axisSteps.end() - planned.begin());
    retry_.emplace(AxisRetry{first, end, positions_});
    reviewPreview_.reset();
    rewindCalibration(first);
    calibrationMessage_.clear();
    updateCalibrationView();
    SetFocus(measure_);
}

void App::cancelCurrentCalibration()
{
    if (!calibrating_)
        return;

    capture_.reset();
    candidates_.clear();
    if (retry_) {
        positions_ = std::move(retry_->positions);
        enterCalibrationReview();
        return;
    }
    calibrating_ = false;
    reviewPreview_.reset();
    calibrationMessage_ = tr(Text::guideCancelled);
    updateCalibrationView();
    SetFocus(calibrationScopeButton_);
}

void App::enterCalibrationReview()
{
    retry_.reset();
    step_ = stepCount_;
    reviewPreview_.emplace(measured_);
    updateLiveControls({});
    updateCalibrationView();
    SetFocus(measure_);
}

void App::rewindCalibration(std::size_t target)
{
    const std::array axes{&measured_.steering, &measured_.throttle, &measured_.brake};
    const auto end = retry_ ? retry_->end : stepCount_;
    for (std::size_t i = target; i < end; ++i) {
        positions_[i] = {};
        if (!retry_ && steps_[i].position == Position::Positive)
            *axes[steps_[i].channel] = {};
    }
    candidates_.clear();
    step_ = target;
}

void App::finishCapture(const chrome_axis::AxisCapture& sample)
{
    const auto step = steps_[step_];
    calibrationMessage_.clear();
    positions_[step_] = sample;
    if (step.position == Position::Positive) {
        const bool steering = step.channel == 0;
        const std::size_t first = step_ - (steering ? 2 : 1);
        candidates_ = chrome_axis::axisCandidates(positions_[first], positions_[step_ - 1], sample, steering);
        if (candidates_.empty()) {
            rewindCalibration(first);
            calibrationMessage_ = tr(Text::mainNoStableMovement);
            updateCalibrationView();
            SetFocus(measure_);
            return;
        }
        if (candidates_.size() == 1) {
            const auto candidate = candidates_.front();
            acceptAxis(candidate);
            return;
        }
        SendMessageW(axisChoice_, CB_RESETCONTENT, 0, 0);
        for (const auto& candidate : candidates_)
            addChoice(axisChoice_, wide(chrome_axis::axisName(candidate.axis)) +
                tr(Text::mainTravelPrefix) + std::to_wstring(static_cast<long long>(candidate.travel)));
        SendMessageW(axisChoice_, CB_SETCURSEL, 0, 0);
        updateCalibrationView();
        SetFocus(axisChoice_);
        return;
    }
    ++step_;
    updateCalibrationView();
    SetFocus(measure_);
}

void App::acceptAxis(const chrome_axis::AxisCandidate& candidate)
{
    const auto channel = steps_[step_].channel;
    const std::size_t first = step_ - (channel == 0 ? 2 : 1);
    auto axis = chrome_axis::makeCalibration(deviceId(channel), positions_[first], positions_[step_ - 1],
                                             positions_[step_], candidate);
    const std::array axes{&measured_.steering, &measured_.throttle, &measured_.brake};
    candidates_.clear();
    for (std::size_t i = 0; i < axes.size(); ++i) {
        if (i == channel)
            continue;
        const bool compatible = i > 0 && channel > 0
            ? chrome_axis::compatibleAxisPair(*axes[i], axis)
            : axes[i]->deviceId != axis.deviceId || axes[i]->axis != axis.axis;
        if (!compatible) {
            rewindCalibration(first);
            calibrationMessage_ = tr(Text::mainAxisAlreadyAssigned);
            updateCalibrationView();
            SetFocus(measure_);
            return;
        }
    }
    *axes[channel] = std::move(axis);
    ++step_;
    if (step_ == (retry_ ? retry_->end : stepCount_)) {
        enterCalibrationReview();
        return;
    }
    updateCalibrationView();
    SetFocus(measure_);
}

void App::finishCalibration()
{
    calibrating_ = false;
    reviewPreview_.reset();
    const std::array axes{&measured_.steering, &measured_.throttle, &measured_.brake};
    for (std::size_t i = 0; i < axes.size(); ++i) {
        if (calibrationScope_[i])
            draft_[i] = std::move(*axes[i]);
    }
    dirty_ = true;
    profileMessage_.clear();
    calibrationMessage_ = tr(Text::mainCalibrationComplete);
    updateProfileView(calibrationScope_);
    updateCalibrationView();
    SetFocus(IsWindowEnabled(save_) ? save_ : calibrationScopeButton_);
}

void App::tick()
{
    publisher_.heartbeat();
    if (!IsWindowEnabled(window_))
        return;
    if (simulation_ && publisher_.enabled() && GetTickCount64() >= nextSimulationStatus_)
        updateSimulationStatus();
    const bool foreground = GetForegroundWindow() == window_ && !IsIconic(window_);
    if (!simulation_ && foreground) {
        if (deviceRefreshPending_)
            scanDevices();
        else if (std::chrono::steady_clock::now() >= nextUsbRefresh_)
            refreshUsbToggle();
    }
    updateCalibrationMotion(foreground);
    if (capture_ && foreground) {
        const auto channel = steps_[step_].channel;
        const auto sample = input_->sample(deviceId(channel));
        ++captureTicks_;
        if ((sample && !capture_->add(*sample)) || captureTicks_ >= 40) {
            capture_.reset();
            calibrationMessage_ = tr(Text::mainReadInterrupted);
            updateCalibrationView();
            SetFocus(measure_);
        } else if (!sample) {
            capture_.emplace();
        } else if (capture_->complete()) {
            const auto result = capture_->result();
            capture_.reset();
            if (result)
                finishCapture(*result);
        }
        if (capture_) {
            RECT client{};
            GetClientRect(window_, &client);
            const auto track = captureProgressBounds(contentBounds(client.right).first);
            InvalidateRect(window_, &track, FALSE);
        }
        return;
    }
    auto* reader = reviewPreview_ ? &*reviewPreview_ : !calibrating_ && preview_ ? &*preview_ : nullptr;
    updateLiveControls(simulation_ ? previewSimulation()
        : reader && foreground ? reader->poll(*input_) : chrome_axis::ControlState{});
}

void App::updateLiveControls(const chrome_axis::ControlState& state)
{
    const std::array current{state.values.steering, state.values.throttle, state.values.brake};
    const std::array ready{state.steeringReady, state.throttleReady, state.brakeReady};
    for (std::size_t i = 0; i < live_.size(); ++i) {
        if (live_[i] == current[i] && liveReady_[i] == ready[i])
            continue;
        const bool changedReady = liveReady_[i] != ready[i];
        const auto percent = std::lround(100 * current[i]);
        const bool changedPercent = percent != std::lround(100 * live_[i]);
        const auto span = static_cast<float>(meterSpans_[i]);
        const bool changedPixels = std::lround(current[i] * span) != std::lround(live_[i] * span);
        live_[i] = current[i];
        liveReady_[i] = ready[i];
        if (changedReady || (ready[i] && changedPercent))
            setText(values_[i], ready[i] ? std::to_wstring(percent) + tr(Text::mainPercentSuffix) : L"—");
        if (changedReady || (ready[i] && changedPixels))
            InvalidateRect(meters_[i], nullptr, FALSE);
    }
}

void App::suspendHardware()
{
    if (input_)
        input_->close();
    if (preview_)
        preview_->reset();
    if (reviewPreview_)
        reviewPreview_->reset();
    if (capture_) {
        capture_.reset();
        calibrationMessage_ = tr(Text::mainMeasureInterrupted);
        updateCalibrationView();
    }
    updateCalibrationMotion(false);
}

void App::contextMenu(int x, int y)
{
    const HMENU menu = CreatePopupMenu();
    if (simulation_) {
        AppendMenuW(menu, MF_STRING, toggleSimulation,
                    publisher_.enabled() ? tr(Text::mainStopSimulation) : tr(Text::mainEnableInGame));
        AppendMenuW(menu, MF_STRING, hardwareTab, tr(Text::mainShowUsbHardware));
    } else {
        refreshUsbToggle();
        AppendMenuW(menu, MF_STRING | (canToggleUsb() ? MF_ENABLED : MF_GRAYED), toggleUsb, usbActionLabel());
        AppendMenuW(menu, MF_STRING, refreshDevices, tr(Text::mainRefreshDevices));
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED),
                    configureAccessories, tr(Text::mainAccessories));
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED),
                    configureFeedback, tr(Text::mainForceFeedback));
        AppendMenuW(menu, MF_STRING | (GetMenuState(deviceMenu_, startCalibration, MF_BYCOMMAND) & MF_GRAYED),
                    startCalibration, tr(Text::mainCalibrateAxes));
        AppendMenuW(menu, MF_STRING | (GetMenuState(deviceMenu_, chooseCalibrationScope, MF_BYCOMMAND) & MF_GRAYED),
                    chooseCalibrationScope, tr(Text::mainCalibrationScope));
        AppendMenuW(menu, MF_STRING | (GetMenuState(deviceMenu_, reassignDevice, MF_BYCOMMAND) & MF_GRAYED),
                    reassignDevice, tr(Text::mainMenuReassignDevice));
        AppendMenuW(menu, MF_STRING | (IsWindowEnabled(save_) ? MF_ENABLED : MF_GRAYED), saveProfile, tr(Text::mainSaveProfile));
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED), selectProfile, tr(Text::mainSelectProfile));
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED), simulationTab, tr(Text::mainShowSimulation));
    }
    if (x == -1 && y == -1) {
        POINT origin{px(24), px(100)};
        ClientToScreen(window_, &origin);
        x = origin.x;
        y = origin.y;
    }
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        x, y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command)
        action(static_cast<int>(command), 0);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->handle(window, message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show)
{
    const auto directory = chrome_axis::calibrationProfileDirectory(chrome_axis::modulePath(instance));
    chrome_axis::initializeLanguage(commandLine ? std::wstring_view(commandLine) : std::wstring_view{},
        deployedProfileDirectory(directory) ? directory : std::filesystem::path{});
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (directory.empty()) {
        MessageBoxW(nullptr, tr(Text::mainProfileFolderUnavailable), title, MB_OK | MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const auto dpi = GetDpiForSystem();
    const auto largeIcon = loadLogo(instance, GetSystemMetricsForDpi(SM_CXICON, dpi));
    const auto smallIcon = loadLogo(instance, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = windowProc;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = largeIcon ? largeIcon.get() : LoadIconW(nullptr, IDI_APPLICATION);
    type.hIconSm = smallIcon ? smallIcon.get() : type.hIcon;
    type.lpszClassName = windowClass;
    if (!RegisterClassExW(&type))
        return 1;

    RECT size{0, 0, MulDiv(862, static_cast<int>(dpi), 96), MulDiv(622, static_cast<int>(dpi), 96)};
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&size, style, TRUE, WS_EX_CONTROLPARENT, dpi);
    App app(directory);
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
        nullptr, nullptr, instance, &app);
    if (!window)
        return 1;
    ShowWindow(window, show);
    UpdateWindow(window);

    ACCEL keys[]{{FVIRTKEY, VK_F5, refreshDevices}, {FVIRTKEY | FCONTROL, 'S', saveProfile}};
    const auto accelerators = CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys)));
    MSG message{};
    int result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!TranslateAcceleratorW(window, accelerators, &message) && !IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DestroyAcceleratorTable(accelerators);
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
