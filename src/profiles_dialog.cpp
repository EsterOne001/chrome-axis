#include "chrome_axis/profiles_dialog.hpp"

#include "dialog_theme.hpp"

#include <iterator>

namespace chrome_axis {
namespace {

struct Picker {
    const std::vector<std::wstring>& names;
    std::wstring_view active;
    std::optional<ProfileChoice> choice;
};

INT_PTR CALLBACK profileDialogProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG) {
        const auto picker = reinterpret_cast<Picker*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        DialogTheme::attach(window, L"Profils", 0, 0, {IDOK});

        const auto combo = GetDlgItem(window, IDC_PROFILE_LIST);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Profil par défaut"));
        LRESULT selected = 0;
        for (std::size_t i = 0; i < picker->names.size(); ++i) {
            const auto& name = picker->names[i];
            const auto row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            if (row >= 0) {
                SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(i + 1));
                if (name == picker->active)
                    selected = row;
            }
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        SendDlgItemMessageW(window, IDC_PROFILE_NAME, EM_SETLIMITTEXT, 64, 0);
        EnableWindow(GetDlgItem(window, IDC_PROFILE_COPY), FALSE);
        return TRUE;
    }

    if (message != WM_COMMAND)
        return FALSE;

    const auto picker = reinterpret_cast<Picker*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!picker)
        return FALSE;
    switch (LOWORD(wp)) {
    case IDCANCEL:
        EndDialog(window, IDCANCEL);
        return TRUE;
    case IDC_PROFILE_NAME:
        if (HIWORD(wp) == EN_CHANGE) {
            EnableWindow(GetDlgItem(window, IDC_PROFILE_COPY),
                         GetWindowTextLengthW(GetDlgItem(window, IDC_PROFILE_NAME)) > 0);
        }
        return TRUE;
    case IDC_PROFILE_COPY: {
        wchar_t name[65]{};
        if (GetDlgItemTextW(window, IDC_PROFILE_NAME, name, static_cast<int>(std::size(name))) == 0)
            return TRUE;
        picker->choice = ProfileChoice{name, true};
        EndDialog(window, IDOK);
        return TRUE;
    }
    case IDOK: {
        const auto row = SendDlgItemMessageW(window, IDC_PROFILE_LIST, CB_GETCURSEL, 0, 0);
        if (row < 0)
            return TRUE;
        const auto index = SendDlgItemMessageW(window, IDC_PROFILE_LIST, CB_GETITEMDATA, static_cast<WPARAM>(row), 0);
        if (index < 0 || static_cast<std::size_t>(index) > picker->names.size())
            return TRUE;
        picker->choice = ProfileChoice{index == 0 ? L"" : picker->names[static_cast<std::size_t>(index - 1)], false};
        EndDialog(window, IDOK);
        return TRUE;
    }
    }
    return FALSE;
}

} // namespace

std::optional<ProfileChoice> chooseProfile(HWND owner, const std::vector<std::wstring>& names, std::wstring_view active)
{
    Picker picker{names, active, {}};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROFILES), owner, profileDialogProc,
                    reinterpret_cast<LPARAM>(&picker));
    return picker.choice;
}

} // namespace chrome_axis
