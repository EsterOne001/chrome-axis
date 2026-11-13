#include "chrome_axis/profiles_dialog.hpp"

#include "app_text.hpp"
#include "dialog_theme.hpp"

#include <shobjidl.h>

#include <iterator>
#include <memory>

namespace chrome_axis {
namespace {

constexpr auto releaseCom = [](auto* object) { object->Release(); };

HRESULT pickFolder(HWND owner, const wchar_t* title, const std::filesystem::path& initial,
                   std::filesystem::path& result)
{
    IFileOpenDialog* dialogPointer{};
    auto status = CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialogPointer));
    if (FAILED(status))
        return status;
    const std::unique_ptr<IFileOpenDialog, decltype(releaseCom)> dialog(dialogPointer);

    DWORD options{};
    if (FAILED(status = dialog->GetOptions(&options)) ||
        FAILED(status = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST)) ||
        FAILED(status = dialog->SetTitle(title)))
        return status;

    IShellItem* initialItem{};
    if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                                  IID_PPV_ARGS(&initialItem)))) {
        const std::unique_ptr<IShellItem, decltype(releaseCom)> folder(initialItem);
        dialog->SetFolder(folder.get());
    }
    if (FAILED(status = dialog->Show(owner)))
        return status;

    IShellItem* selectedItem{};
    if (FAILED(status = dialog->GetResult(&selectedItem)))
        return status;
    const std::unique_ptr<IShellItem, decltype(releaseCom)> item(selectedItem);
    PWSTR path{};
    if (FAILED(status = item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return status;
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedPath(path, CoTaskMemFree);
    result = ownedPath.get();
    return S_OK;
}

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
        SetWindowTextW(window, tr(Text::DlgProfilesTitle));
        SetDlgItemTextW(window, IDC_PROFILE_LIST_LABEL, tr(Text::DlgProfilesToLoad));
        SetDlgItemTextW(window, IDC_PROFILE_NAME_LABEL, tr(Text::DlgProfilesCopyName));
        SetDlgItemTextW(window, IDC_PROFILE_COPY, tr(Text::DlgProfilesCopy));
        SetDlgItemTextW(window, IDOK, tr(Text::DlgProfilesLoad));
        SetDlgItemTextW(window, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(window, tr(Text::DlgProfilesHeading), 0, 0, {IDOK});

        const auto combo = GetDlgItem(window, IDC_PROFILE_LIST);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr(Text::DlgProfilesDefault)));
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

HRESULT chooseProfileFolder(HWND owner, const wchar_t* title, const std::filesystem::path& initial,
                            std::filesystem::path& result)
{
    const auto status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(status))
        return status;
    const auto picked = pickFolder(owner, title, initial, result);
    CoUninitialize();
    return picked;
}

} // namespace chrome_axis
