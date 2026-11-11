#include "chrome_axis/accessories.hpp"
#include "chrome_axis/accessories_dialog.hpp"
#include "chrome_axis/calibration.hpp"
#include "chrome_axis/configuration.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/feedback_dialog.hpp"
#include "chrome_axis/presets.hpp"
#include "chrome_axis/profiles_dialog.hpp"
#include "chrome_axis/simulation.hpp"
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
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using namespace chrome_axis::ui;

constexpr wchar_t windowClass[] = L"ChromeAxisCalibration";
constexpr wchar_t title[] = L"Chrome Axis";
constexpr UINT_PTR inputTimer = 1;
constexpr UINT_PTR deviceTimer = 2;
constexpr int contentWidthLimit = 1120;
constexpr std::array commands{L"Direction", L"Accélérateur", L"Frein"};
constexpr std::array deviceLabels{L"Volant", L"Accélérateur", L"Frein"};
constexpr std::array sharedDeviceLabels{L"", L"Même que le volant", L"Même que l’accélérateur"};
constexpr std::array keyLabels{L"Pavé 4 / 6  ·  F6 / F7", L"Pavé 8  ·  F8", L"Pavé 2  ·  F10"};
constexpr std::string_view sameAsPrevious = "*";

enum Command {
    deviceComboFirst = 100, throttleCombo, brakeCombo, refreshDevices, configureAccessories,
    startCalibration, measure, cancelCalibration, chooseAxis,
    saveProfile, reloadProfile, openProfileFolder, closeApp, aboutApp, hardwareTab, simulationTab, toggleSimulation,
    toggleUsb, selectProfile, configureFeedback, deadzoneFirst = 200, meterFirst = 210
};

enum class Position { Neutral, Negative, Positive };

struct Step {
    std::size_t channel;
    Position position;
    const wchar_t* instruction;
};

constexpr std::array steps{
    Step{0, Position::Neutral, L"Placez le volant au centre et gardez-le immobile."},
    Step{0, Position::Negative, L"Braquez complètement à gauche et maintenez le volant."},
    Step{0, Position::Positive, L"Braquez complètement à droite et maintenez le volant."},
    Step{1, Position::Neutral, L"Relâchez les pédales et ramenez le volant au centre."},
    Step{1, Position::Positive, L"Enfoncez uniquement l’accélérateur à fond et maintenez-le."},
    Step{2, Position::Neutral, L"Relâchez les pédales."},
    Step{2, Position::Positive, L"Enfoncez uniquement le frein à fond et maintenez-le."}
};

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const auto length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (size == 0)
        return L"Texte indisponible";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size);
    return result;
}

bool hasAxes(const chrome_axis::DeviceInfo& device)
{
    return std::ranges::any_of(device.axes, [](bool present) { return present; });
}

std::wstring formatDeadzone(float value)
{
    const auto tenths = std::lround(1000 * value);
    auto result = std::to_wstring(tenths / 10);
    if (tenths % 10)
        result += L"," + std::to_wstring(tenths % 10);
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
        : directory_(std::move(directory)) {}
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
            SetTextColor(dc, heading ? yellow : value ? cyan : muted ? secondary : ink);
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
            if (!dirty_ || MessageBoxW(window, L"Fermer sans enregistrer les modifications du profil ?",
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
        AppendMenuW(profileMenu_, MF_STRING, saveProfile, L"&Enregistrer\tCtrl+S");
        AppendMenuW(profileMenu_, MF_STRING, reloadProfile, L"&Recharger");
        AppendMenuW(profileMenu_, MF_STRING, selectProfile, L"&Sélectionner un profil…");
        AppendMenuW(profileMenu_, MF_STRING, openProfileFolder, L"Ouvrir le &dossier des profils");
        AppendMenuW(profileMenu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(profileMenu_, MF_STRING, closeApp, L"&Quitter");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(profileMenu_), L"&Profil");

        deviceMenu_ = CreatePopupMenu();
        AppendMenuW(deviceMenu_, MF_STRING, refreshDevices, L"&Actualiser\tF5");
        AppendMenuW(deviceMenu_, MF_STRING, startCalibration, L"&Calibrer les axes");
        AppendMenuW(deviceMenu_, MF_STRING, configureAccessories, L"Accessoi&res…");
        AppendMenuW(deviceMenu_, MF_STRING, configureFeedback, L"Retour de &force…");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(deviceMenu_), L"&Périphériques");
        const HMENU help = CreatePopupMenu();
        AppendMenuW(help, MF_STRING, aboutApp, L"&À propos");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Aide");
        SetMenu(window_, menu);

        hardwareTab_ = button(L"MATÉRIEL USB", hardwareTab);
        simulationTab_ = button(L"SIMULATION", simulationTab);
        devicesTitle_ = label(L"Périphériques");
        usbToggle_ = button(usbActionLabel(), toggleUsb);
        refresh_ = button(L"Actualiser", refreshDevices);
        accessories_ = button(L"Accessoires…", configureAccessories);
        for (std::size_t i = 0; i < deviceLabels.size(); ++i) {
            deviceLabels_[i] = label(deviceLabels[i]);
            deviceCombos_[i] = combo(deviceComboFirst + static_cast<int>(i));
        }
        hardware_ = label(L"Recherche de périphériques…");
        simToggle_ = button(L"Activer dans le jeu", toggleSimulation);
        simState_ = label(L"");
        simHint_ = label(L"Les jauges réagissent quand cette fenêtre a le focus.");

        axesTitle_ = label(L"Axes du véhicule");
        headings_ = {label(L"Commande"), label(L"Axe détecté"), label(L"Zone morte (%)"), label(L"Aperçu local")};
        for (std::size_t i = 0; i < commands.size(); ++i) {
            names_[i] = label(commands[i]);
            bindings_[i] = label(L"À calibrer");
            zones_[i] = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                              deadzoneFirst + static_cast<int>(i), WS_EX_CLIENTEDGE);
            SendMessageW(zones_[i], EM_SETLIMITTEXT, 5, 0);
            meters_[i] = child(L"STATIC", L"", SS_OWNERDRAW, meterFirst + static_cast<int>(i));
            values_[i] = label(L"—");
            simKeys_[i] = label(keyLabels[i]);
        }

        calibrationTitle_ = label(L"Calibration");
        feedback_ = button(L"Retour de force…", configureFeedback);
        instruction_ = label(L"Connectez votre matériel pour commencer.");
        start_ = button(L"Calibrer les axes", startCalibration);
        axisChoice_ = combo(chooseAxis);
        measure_ = button(L"Mesurer", measure);
        cancel_ = button(L"Annuler", cancelCalibration);
        simDirections_ = label(L"Gardez cette fenêtre ouverte, retournez dans le jeu et installez V au volant.");
        simArming_ = label(L"Relâchez les touches de simulation, puis maintenez une commande pour faire monter l’axe.");
        profileName_ = child(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0);
        status_ = label(L"Aucun profil enregistré");
        folder_ = button(L"Dossier des profils", openProfileFolder);
        select_ = button(L"Sélectionner un profil…", selectProfile);
        reload_ = button(L"Recharger", reloadProfile);
        save_ = button(L"Enregistrer le profil", saveProfile);

        input_ = std::make_unique<chrome_axis::DirectInput>(window_);
        updateDpiResources();
        readProfile();
        scanDevices();
        applyMode();
        updateCalibrationView();
        layout();
        if (!SetTimer(window_, inputTimer, 30, nullptr)) {
            MessageBoxW(window_, L"Impossible de démarrer la lecture des commandes. Relancez Chrome Axis.",
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
        for (const auto control : {devicesTitle_, axesTitle_, calibrationTitle_, hardwareTab_, simulationTab_})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_[1]), FALSE);
        for (const auto control : deviceCombos_)
            SendMessageW(control, CB_SETITEMHEIGHT, 0, px(24));
        SendMessageW(axisChoice_, CB_SETITEMHEIGHT, 0, px(24));
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
        placements.reserve(53);
        const auto place = [&](HWND control, int x, int y, int w, int h) {
            if (control)
                placements.push_back({control, px(left + x), px(y), px(w), px(h)});
        };

        place(hardwareTab_, width - 314, 26, 148, 36);
        place(simulationTab_, width - 156, 26, 132, 36);
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
        const int meterWidth = width - column[3] - 112;
        const int pixelWidth = px(meterWidth);
        meterSpans_ = {pixelWidth - pixelWidth / 2, pixelWidth, pixelWidth};
        for (std::size_t i = 0; i < column.size(); ++i)
            place(headings_[i], column[i], 278, headingWidths[i], 20);
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const int y = 303 + static_cast<int>(i) * 32;
            place(names_[i], column[0], y + 3, 135, 22);
            place(bindings_[i], column[1], y + 3, 105, 22);
            place(simKeys_[i], column[1], y + 3, 226, 22);
            place(zones_[i], column[2], y, 62, 24);
            place(meters_[i], column[3], y + 8, meterWidth, 11);
            place(values_[i], width - 102, y + 3, 60, 22);
        }

        place(calibrationTitle_, 42, 430, 340, 24);
        place(feedback_, width - 196, 428, 154, 29);
        place(instruction_, 42, 459, width - 84, 24);
        place(start_, 42, 491, 158, 29);
        place(axisChoice_, 42, 492, 198, 180);
        place(measure_, 250, 491, 135, 29);
        place(cancel_, 395, 491, 100, 29);
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
            const auto buffer = CreateCompatibleDC(dc);
            const auto bitmap = buffer ? CreateCompatibleBitmap(dc, width, height) : nullptr;
            if (bitmap) {
                const auto previous = SelectObject(buffer, bitmap);
                SetViewportOrgEx(buffer, -region.left, -region.top, nullptr);
                paintScene(buffer, client);
                SetViewportOrgEx(buffer, 0, 0, nullptr);
                BitBlt(dc, region.left, region.top, width, height, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, previous);
                DeleteObject(bitmap);
            } else {
                paintScene(dc, client);
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
        paintHeader(dc, dpi_, client.right, left + 24, px(left + width - 312), headerLogo_.get(),
                    fonts_[0], fonts_[2], L"Calibration · Cyberpunk 2077 2.5");

        for (const auto& [top, bottom] : {std::pair{98, 222}, {235, 405}, {418, 535}}) {
            const RECT card{px(left + 22), px(top), px(left + width - 22), px(bottom)};
            cutPanel(dc, card, px(13), highContrast ? GetSysColor(COLOR_WINDOW) : panel,
                     highContrast ? GetSysColor(COLOR_WINDOWTEXT) : edge);
            fill(dc, {card.left, card.top + px(12), card.left + px(2), card.top + px(33)},
                 highContrast ? GetSysColor(COLOR_HIGHLIGHT) : cyan);
        }
        const LONG footerRule = px(542);
        fill(dc, {px(left + 22), footerRule, px(left + width - 22), footerRule + 1},
             highContrast ? GetSysColor(COLOR_WINDOWTEXT) : edge);
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
    void updateProfileView();
    void rebuildPreview();
    bool ensureProfileDirectory(const std::filesystem::path& directory);
    bool confirmPersistentDirectory() const;
    bool confirmCurrentPreset();
    void chooseStoredProfile();
    void persist();
    void updateCalibrationView();
    void refreshUsbToggle();
    void changeUsbMode();
    void setMode(bool simulation);
    void applyMode();
    void updateSimulationView();
    chrome_axis::ControlState previewSimulation();
    void beginCalibration();
    void skipUnassignedSteps();
    void measureStep();
    void finishCapture(const chrome_axis::AxisCapture& capture);
    void acceptAxis(const chrome_axis::AxisCandidate& candidate);
    void suspendHardware();
    void tick();
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
    bool canCalibrate() const
    {
        bool assigned = false;
        for (std::size_t i = 0; i < deviceSelections_.size(); ++i) {
            const auto& id = deviceId(i);
            if (id.empty())
                continue;
            assigned = true;
            const auto* selected = device(id);
            if (!selected || !hasAxes(*selected))
                return false;
        }
        return assigned;
    }
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
            return L"Activer l’USB en jeu";
        return installed_ ? L"Désactiver l’USB en jeu" : L"Retirer l’activation locale";
    }
    const std::filesystem::path& activeDirectory() const { return preset_.directory; }

    std::filesystem::path directory_;
    std::filesystem::path path_;
    chrome_axis::ProfilePreset preset_;
    HWND window_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    std::array<HFONT, 3> fonts_{};
    Icon headerLogo_;
    HBRUSH backgroundBrush_{}, panelBrush_{}, fieldBrush_{};
    HMENU profileMenu_{}, deviceMenu_{};
    HWND devicesTitle_{}, axesTitle_{}, calibrationTitle_{};
    HWND accessories_{}, feedback_{}, refresh_{}, hardware_{}, instruction_{}, start_{}, axisChoice_{}, measure_{}, cancel_{};
    HWND profileName_{}, status_{}, folder_{}, select_{}, reload_{}, save_{};
    HWND hardwareTab_{}, simulationTab_{}, usbToggle_{}, simToggle_{}, simState_{}, simHint_{}, simDirections_{}, simArming_{};
    std::array<HWND, 4> headings_{};
    std::array<HWND, 3> deviceLabels_{}, deviceCombos_{};
    std::array<HWND, 3> names_{}, bindings_{}, zones_{}, meters_{}, values_{};
    std::array<HWND, 3> simKeys_{};
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

    bool calibrating_{};
    std::size_t step_{};
    std::optional<chrome_axis::CalibrationCapture> capture_;
    unsigned int captureTicks_{};
    chrome_axis::AxisCapture neutral_{}, negative_{}, positive_{};
    std::vector<chrome_axis::AxisCandidate> candidates_;
    chrome_axis::Profile measured_{};
    std::wstring calibrationMessage_;
    bool simulation_{};
    bool publisherFailed_{};
    chrome_axis::SimulationPublisher publisher_;
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
            if (publisher_.enabled())
                publisher_.disable();
            else
                publisherFailed_ = !publisher_.enable();
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
    case startCalibration:
        beginCalibration();
        break;
    case measure:
        measureStep();
        break;
    case cancelCalibration:
        calibrating_ = false;
        capture_.reset();
        candidates_.clear();
        calibrationMessage_.clear();
        updateCalibrationView();
        break;
    case saveProfile:
        persist();
        break;
    case reloadProfile:
        if (!simulation_ && !calibrating_ && (!dirty_ || MessageBoxW(window_,
                L"Recharger le profil actif et abandonner les modifications non enregistrées ?",
                title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)) {
            readProfile();
            scanDevices();
        }
        break;
    case selectProfile:
        chooseStoredProfile();
        break;
    case openProfileFolder:
        if (ensureProfileDirectory(directory_) && reinterpret_cast<INT_PTR>(
                ShellExecuteW(window_, L"open", directory_.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            MessageBoxW(window_, L"Impossible d’ouvrir le dossier des profils.", title, MB_OK | MB_ICONERROR);
        }
        break;
    case closeApp:
        SendMessageW(window_, WM_CLOSE, 0, 0);
        break;
    case aboutApp: {
        const auto description = std::wstring(title) + L" · " CHROME_AXIS_VERSION_WIDE L"\n"
            L"Calibration DirectInput 8 et simulation clavier pour Chrome Axis, "
            L"mod communautaire indépendant pour Cyberpunk 2077 2.5.\n\n"
            L"Les profils sont enregistrés dans le dossier Profil, à côté du mod. "
            L"L’onglet Matériel USB permet de calibrer les axes et d’affecter des touches aux accessoires.";
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
        hardwareStatus = L"DirectInput est indisponible";
        advice = L"Réessayez avec Actualiser.";
    } else if (devices_.empty()) {
        hardwareStatus = L"Aucun périphérique détecté";
        advice = L"Branchez le matériel USB, puis cliquez sur Actualiser.";
    } else if (disconnected) {
        hardwareStatus = L"Périphérique sélectionné déconnecté";
        advice = L"Ses réglages sont conservés.";
    } else {
        hardwareStatus = std::to_wstring(devices_.size()) +
            (devices_.size() == 1 ? L" périphérique DirectInput détecté" : L" périphériques DirectInput détectés");
    }
    if (temporaryDirectory_) {
        hardwareStatus = L"Copie temporaire · Extrayez le ZIP ou ouvrez l’application depuis le jeu.";
        advice = {};
    } else if (!installed_)
        advice = L"Ouvrez l’application depuis le dossier du jeu pour l’USB.";
    if (!advice.empty()) {
        hardwareStatus += L". ";
        hardwareStatus += advice;
    }
    setText(hardware_, hardwareStatus);
    if (calibrating_ && disconnected) {
        calibrating_ = false;
        capture_.reset();
        candidates_.clear();
        calibrationMessage_ = L"Périphérique déconnecté. Rebranchez-le avant de relancer la calibration.";
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
    addChoice(control, L"Commandes du jeu");
    if (channel > 0) {
        choices.emplace_back(sameAsPrevious);
        addChoice(control, sharedDeviceLabels[channel]);
    }
    for (const auto& item : devices_) {
        if (!hasAxes(item))
            continue;
        const auto duplicates = std::count_if(devices_.begin(), devices_.end(), [&](const auto& other) {
            return hasAxes(other) && item.name == other.name;
        });
        auto name = wide(item.name);
        if (duplicates > 1)
            name += L" [" + wide(item.id.substr(29, 8)) + L"]";
        choices.push_back(item.id);
        addChoice(control, name);
    }
    auto found = std::find(choices.begin(), choices.end(), selection);
    if (found == choices.end()) {
        choices.push_back(selection);
        addChoice(control, L"Périphérique du profil (déconnecté)");
        found = std::prev(choices.end());
    }
    SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(found - choices.begin()), 0);
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
            profileMessage_ = L"Profil actif introuvable. Sélectionnez un profil.";
        } else if (std::filesystem::exists(path_, ec) || ec) {
            profileMessage_ = L"Fichier de calibration illisible ou invalide.";
        }
    }
    setText(profileName_, activeDirectory().empty() ? L"Profil indisponible"
        : preset_.name.empty() ? L"Profil par défaut" : L"Profil · " + preset_.name);
    readAccessories();
    updateProfileView();
}

void App::updateProfileView()
{
    loadingControls_ = true;
    for (std::size_t i = 0; i < draft_.size(); ++i) {
        setText(bindings_[i], draft_[i] ? wide(chrome_axis::axisName(draft_[i]->axis)) : L"À calibrer");
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
        : !zonesValid ? L"Zone morte : saisissez de 0,5 à 25 %."
        : !pedalsCompatible ? L"Zones mortes incompatibles avec l’axe combiné."
        : dirty_ && clearAnalogProfile() ? L"Enregistrez pour retirer la calibration."
        : dirty_ ? (canSave ? L"Enregistrez le profil pour appliquer vos réglages" : L"Calibration incomplète")
        : saved_ ? L"Calibration enregistrée"
        : accessoriesSaved_ ? L"Affectations d’accessoires enregistrées"
        : L"Aucun profil enregistré");
}

bool App::ensureProfileDirectory(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (!error)
        return true;
    MessageBoxW(window_, L"Impossible de créer le dossier Profil de Chrome Axis.",
        title, MB_OK | MB_ICONERROR);
    return false;
}

bool App::confirmPersistentDirectory() const
{
    if (!temporaryDirectory_)
        return true;
    MessageBoxW(window_, L"Cette copie se trouve dans un dossier temporaire. Extrayez le ZIP dans un dossier "
        L"durable ou ouvrez Chrome Axis depuis le dossier du jeu avant d’enregistrer.",
        title, MB_OK | MB_ICONWARNING);
    return false;
}

bool App::confirmCurrentPreset()
{
    const auto current = chrome_axis::activePreset(directory_);
    if (!current.directory.empty() && current.directory == activeDirectory())
        return true;

    MessageBoxW(window_, current.directory.empty() ? L"Sélectionnez d’abord un profil disponible."
        : L"Le profil actif a changé. Cliquez sur Recharger pour le charger.", title, MB_OK | MB_ICONWARNING);
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
            L"Enregistrer les modifications de la calibration avant de changer de profil ?",
            title, MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (response == IDYES) {
            if (!canSaveProfile()) {
                MessageBoxW(window_, L"Terminez la calibration ou choisissez de continuer sans l’enregistrer.",
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
        MessageBoxW(window_, wide(error).c_str(), title, MB_OK | MB_ICONWARNING);
        return;
    }
    readProfile();
    scanDevices();
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
                MessageBoxW(window_, L"Impossible de désactiver le retour de force de l’ancien axe. "
                    L"Vérifiez les droits d’écriture avant de changer le volant ou l’axe.",
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
            ? L"Le profil n’a pas pu être enregistré. Le retour de force a été désactivé pour ce profil."
            : L"Le profil n’a pas pu être enregistré. Vérifiez que le dossier Profil est accessible en écriture.",
            title, MB_OK | MB_ICONERROR);
        return;
    }
    dirty_ = false;
    saved_ = previewProfile_.has_value();
    savedSteering_ = nextSteering;
    refreshUsbToggle();
    profileMessage_ = !saved_ ? L"Les axes utilisent les commandes du jeu."
        : resetFeedback ? L"Profil enregistré · vérifiez le retour de force pour le nouvel axe."
        : !installed_ ? L"Profil enregistré localement"
        : usbEnabled_ ? L"Profil enregistré · activation USB demandée"
        : L"Profil enregistré · activez l’USB en jeu";
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
        if (!chrome_axis::isInstalledPluginDirectory(directory_.parent_path())) {
            const auto message = L"L’application utilise actuellement ce dossier de profil.\n\n" + directory_.wstring()
                + L"\n\nDéployez le mod dans Vortex, puis ouvrez ChromeAxis.exe depuis red4ext\\plugins\\chrome_axis "
                L"dans le dossier de Cyberpunk 2077.";
            MessageBoxW(window_, message.c_str(), title, MB_OK | MB_ICONWARNING);
            return;
        }
        if (dirty_ || (saved_ && !previewProfile_)) {
            const auto message = canSaveProfile()
                ? L"Enregistrez vos réglages ou rechargez le profil enregistré, puis activez l’USB en jeu."
                : L"Terminez la calibration ou rechargez le profil enregistré, puis activez l’USB en jeu.";
            MessageBoxW(window_, message, title, MB_OK | MB_ICONWARNING);
            return;
        }
        if (!accessoriesSaved_ && !chrome_axis::loadProfile(path_, error)) {
            MessageBoxW(window_, L"Enregistrez une calibration valide ou une affectation d’accessoire.",
                title, MB_OK | MB_ICONWARNING);
            return;
        }
    }
    if (!chrome_axis::setUsbEnabled(directory_, enable, error)) {
        MessageBoxW(window_, L"Impossible de modifier l’état de l’USB en jeu.",
            title, MB_OK | MB_ICONERROR);
        return;
    }
    refreshUsbToggle();
    profileMessage_ = enable ? L"USB demandé · fermez Chrome Axis et retournez en jeu"
        : installed_ ? L"USB désactivé en jeu" : L"Activation USB locale retirée";
    setText(status_, profileMessage_);
}

void App::updateCalibrationView()
{
    if (simulation_) {
        updateSimulationView();
        return;
    }
    refreshUsbToggle();
    const bool canStart = !calibrating_ && canCalibrate();
    const bool choosing = !candidates_.empty();
    ShowWindow(start_, calibrating_ ? SW_HIDE : SW_SHOW);
    ShowWindow(measure_, calibrating_ ? SW_SHOW : SW_HIDE);
    ShowWindow(cancel_, calibrating_ ? SW_SHOW : SW_HIDE);
    ShowWindow(axisChoice_, calibrating_ && choosing ? SW_SHOW : SW_HIDE);
    EnableWindow(start_, canStart);
    const bool axisDeviceAvailable = std::ranges::any_of(devices_, hasAxes);
    for (std::size_t i = 0; i < deviceCombos_.size(); ++i)
        EnableWindow(deviceCombos_[i], !calibrating_ && (axisDeviceAvailable || !deviceSelections_[i].empty()));
    EnableWindow(accessories_, !calibrating_);
    EnableWindow(feedback_, !calibrating_);
    EnableWindow(reload_, !calibrating_);
    EnableWindow(select_, !calibrating_);
    EnableWindow(simulationTab_, !calibrating_);
    for (std::size_t i = 0; i < zones_.size(); ++i)
        EnableWindow(zones_[i], !calibrating_ && draft_[i].has_value());
    EnableMenuItem(deviceMenu_, startCalibration, MF_BYCOMMAND | (canStart ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(deviceMenu_, refreshDevices, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(deviceMenu_, configureAccessories, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(deviceMenu_, configureFeedback, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(profileMenu_, reloadProfile, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(profileMenu_, selectProfile, MF_BYCOMMAND | (calibrating_ ? MF_GRAYED : MF_ENABLED));
    const bool canSave = canSaveProfile();
    EnableWindow(save_, canSave);
    EnableMenuItem(profileMenu_, saveProfile, MF_BYCOMMAND | (canSave ? MF_ENABLED : MF_GRAYED));

    if (!calibrating_) {
        setText(calibrationTitle_, L"Calibration");
        setText(instruction_, !calibrationMessage_.empty() ? calibrationMessage_
            : canStart ? L"Mesurez la course de chaque axe attribué."
            : !axisDeviceAvailable ? L"Connectez un volant ou des pédales pour calibrer les axes."
            : L"Sélectionnez un périphérique pour les axes à calibrer.");
        return;
    }

    setText(calibrationTitle_, std::wstring(commands[steps[step_].channel]) + L"   ·   " +
        std::to_wstring(step_ + 1) + L" / " + std::to_wstring(steps.size()));
    setText(instruction_, !calibrationMessage_.empty() ? calibrationMessage_
        : choosing ? L"Plusieurs axes ont bougé. Sélectionnez l’axe correspondant à cette commande."
        : steps[step_].instruction);
    setText(measure_, capture_ ? L"Mesure en cours…" : choosing ? L"Choisir cet axe" : L"Mesurer");
    EnableWindow(measure_, !capture_);
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
    for (const auto control : {usbToggle_, refresh_, accessories_, feedback_, hardware_, headings_[2],
                              instruction_, start_, profileName_, folder_, select_, reload_, save_})
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
    setText(devicesTitle_, simulation_ ? L"SIMULATION CLAVIER" : L"PÉRIPHÉRIQUES");
    setText(axesTitle_, simulation_ ? L"SIGNAUX SIMULÉS" : L"AXES DU VÉHICULE");
    setText(headings_[1], simulation_ ? L"Touches" : L"Axe détecté");
    setText(headings_[3], simulation_ ? L"Aperçu en direct" : L"Aperçu local");
    redrawControls();
}

void App::updateSimulationView()
{
    const bool active = publisher_.enabled();
    setText(simToggle_, active ? L"Arrêter la simulation" : L"Activer dans le jeu");
    InvalidateRect(simToggle_, nullptr, FALSE);
    setText(simState_, active ? L"Simulation activée pour Cyberpunk."
        : publisherFailed_ ? L"Activation impossible. Fermez les autres instances."
        : L"Aperçu disponible sans périphérique.");
    setText(calibrationTitle_, L"ESSAI DANS CYBERPUNK");
    setText(status_, active ? L"Mode jeu actif · laissez l’application ouverte"
        : L"Mode aperçu · aucun signal envoyé au jeu");
    EnableMenuItem(deviceMenu_, startCalibration, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, refreshDevices, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, configureAccessories, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(deviceMenu_, configureFeedback, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, saveProfile, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, reloadProfile, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(profileMenu_, selectProfile, MF_BYCOMMAND | MF_GRAYED);
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

void App::beginCalibration()
{
    if (simulation_ || calibrating_ || !canCalibrate())
        return;
    calibrating_ = true;
    step_ = 0;
    skipUnassignedSteps();
    measured_ = {};
    candidates_.clear();
    calibrationMessage_.clear();
    updateCalibrationView();
    SetFocus(measure_);
}

void App::skipUnassignedSteps()
{
    while (step_ < steps.size() && deviceId(steps[step_].channel).empty())
        ++step_;
}

void App::measureStep()
{
    if (!calibrating_ || capture_)
        return;
    if (!candidates_.empty()) {
        const auto selected = SendMessageW(axisChoice_, CB_GETCURSEL, 0, 0);
        if (selected != CB_ERR && static_cast<std::size_t>(selected) < candidates_.size()) {
            const auto candidate = candidates_[static_cast<std::size_t>(selected)];
            acceptAxis(candidate);
        }
        return;
    }
    calibrationMessage_ = L"Gardez la commande immobile pendant la mesure.";
    capture_.emplace();
    captureTicks_ = 0;
    updateCalibrationView();
}

void App::finishCapture(const chrome_axis::AxisCapture& sample)
{
    const auto step = steps[step_];
    calibrationMessage_.clear();
    if (step.position == Position::Neutral) {
        neutral_ = negative_ = sample;
    } else if (step.position == Position::Negative) {
        negative_ = sample;
    } else {
        positive_ = sample;
        candidates_ = chrome_axis::axisCandidates(neutral_, negative_, positive_, step.channel == 0);
        if (candidates_.empty()) {
            step_ -= step.channel == 0 ? 2 : 1;
            calibrationMessage_ = L"Aucun mouvement stable. Remettez la commande au repos et recommencez cette mesure.";
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
                L"  ·  course " + std::to_wstring(static_cast<long long>(candidate.travel)));
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
    const auto channel = steps[step_].channel;
    auto axis = chrome_axis::makeCalibration(deviceId(channel), neutral_, negative_, positive_, candidate);
    const std::array axes{&measured_.steering, &measured_.throttle, &measured_.brake};
    candidates_.clear();
    for (std::size_t i = 0; i < channel; ++i) {
        const bool compatible = i == 1 && channel == 2
            ? chrome_axis::compatibleAxisPair(*axes[i], axis)
            : axes[i]->deviceId != axis.deviceId || axes[i]->axis != axis.axis;
        if (!compatible) {
            --step_;
            calibrationMessage_ = L"Axe déjà attribué. Sur un axe combiné, les pédales doivent partir du même repos en sens opposés.";
            updateCalibrationView();
            SetFocus(measure_);
            return;
        }
    }
    *axes[channel] = std::move(axis);
    ++step_;
    skipUnassignedSteps();
    if (step_ == steps.size()) {
        calibrating_ = false;
        for (std::size_t i = 0; i < axes.size(); ++i) {
            draft_[i].reset();
            if (!axes[i]->deviceId.empty())
                draft_[i] = std::move(*axes[i]);
        }
        dirty_ = true;
        profileMessage_.clear();
        calibrationMessage_ = L"Calibration terminée. Relâchez les commandes, vérifiez les valeurs et enregistrez le profil.";
        updateProfileView();
        updateCalibrationView();
        SetFocus(save_);
    } else {
        updateCalibrationView();
        SetFocus(measure_);
    }
}

void App::tick()
{
    publisher_.heartbeat();
    if (!IsWindowEnabled(window_))
        return;
    const bool foreground = GetForegroundWindow() == window_ && !IsIconic(window_);
    if (!simulation_ && foreground) {
        if (deviceRefreshPending_)
            scanDevices();
        else if (std::chrono::steady_clock::now() >= nextUsbRefresh_)
            refreshUsbToggle();
    }
    if (capture_ && foreground) {
        const auto channel = steps[step_].channel;
        const auto sample = input_->sample(deviceId(channel));
        ++captureTicks_;
        if ((sample && !capture_->add(*sample)) || captureTicks_ >= 40) {
            capture_.reset();
            calibrationMessage_ = L"Lecture interrompue. Reconnectez le périphérique, puis réessayez.";
            updateCalibrationView();
        } else if (!sample) {
            capture_.emplace();
        } else if (capture_->complete()) {
            const auto result = capture_->result();
            capture_.reset();
            if (result)
                finishCapture(*result);
        }
        return;
    }
    const auto state = simulation_ ? previewSimulation()
        : preview_ && !calibrating_ && foreground ? preview_->poll(*input_) : chrome_axis::ControlState{};
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
            setText(values_[i], ready[i] ? std::to_wstring(percent) + L" %" : L"—");
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
    if (capture_) {
        capture_.reset();
        calibrationMessage_ = L"Mesure interrompue. Replacez la commande, puis cliquez sur Mesurer.";
        updateCalibrationView();
    }
}

void App::contextMenu(int x, int y)
{
    const HMENU menu = CreatePopupMenu();
    if (simulation_) {
        AppendMenuW(menu, MF_STRING, toggleSimulation,
                    publisher_.enabled() ? L"Arrêter la simulation" : L"Activer dans le jeu");
        AppendMenuW(menu, MF_STRING, hardwareTab, L"Afficher le matériel USB");
    } else {
        refreshUsbToggle();
        AppendMenuW(menu, MF_STRING | (canToggleUsb() ? MF_ENABLED : MF_GRAYED), toggleUsb, usbActionLabel());
        AppendMenuW(menu, MF_STRING, refreshDevices, L"Actualiser les périphériques");
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED),
                    configureAccessories, L"Accessoires…");
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED),
                    configureFeedback, L"Retour de force…");
        AppendMenuW(menu, MF_STRING | (!calibrating_ && canCalibrate() ? MF_ENABLED : MF_GRAYED),
                    startCalibration, L"Calibrer les axes");
        AppendMenuW(menu, MF_STRING | (IsWindowEnabled(save_) ? MF_ENABLED : MF_GRAYED), saveProfile, L"Enregistrer le profil");
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED), selectProfile, L"Sélectionner un profil…");
        AppendMenuW(menu, MF_STRING | (calibrating_ ? MF_GRAYED : MF_ENABLED), simulationTab, L"Afficher la simulation");
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto directory = chrome_axis::calibrationProfileDirectory(chrome_axis::modulePath(instance));
    if (directory.empty()) {
        MessageBoxW(nullptr, L"Impossible de localiser le dossier Profil de Chrome Axis.", title, MB_OK | MB_ICONERROR);
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
