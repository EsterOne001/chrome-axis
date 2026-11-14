#include "chrome_axis/profiles_dialog.hpp"

#include "app_text.hpp"
#include "dialog_theme.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <array>
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

struct SavedDevice {
    std::string id;
    std::wstring roles;
};

struct DevicePicker {
    const Profile& profile;
    const AccessoryProfile& accessories;
    const std::vector<DeviceInfo>& connected;
    std::vector<SavedDevice> saved;
    std::optional<DeviceReassignment> choice;
};

std::wstring fromDeviceName(const DeviceInfo& device)
{
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.name.c_str(), -1, nullptr, 0);
    if (count <= 1)
        return tr(Text::mainTextUnavailable);
    std::wstring name(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.name.c_str(), -1, name.data(), count);
    name.pop_back();
    return name;
}

std::wstring deviceLabel(std::string_view id, const std::vector<DeviceInfo>& connected)
{
    const auto found = std::ranges::find(connected, id, &DeviceInfo::id);
    if (found != connected.end() &&
        std::ranges::count(connected, found->name, &DeviceInfo::name) == 1) {
        return fromDeviceName(*found);
    }
    const auto shortId = id.substr(id.starts_with('{') ? 1 : 0, 8);
    const auto name = found == connected.end() ? std::wstring(tr(Text::mainReassignUnknownDevice)) : fromDeviceName(*found);
    return name + L" [" + std::wstring(shortId.begin(), shortId.end()) + L"]";
}

void addDeviceRole(std::vector<SavedDevice>& saved, const std::string& id, Text role)
{
    if (id.empty())
        return;
    const auto existing = std::ranges::find(saved, id, &SavedDevice::id);
    if (existing == saved.end())
        saved.push_back({id, tr(role)});
    else
        existing->roles += std::wstring(L", ") + tr(role);
}

void addDeviceRow(HWND dialog, int control, const std::wstring& label, std::size_t index)
{
    const auto combo = GetDlgItem(dialog, control);
    const auto row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (row >= 0)
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(row), static_cast<LPARAM>(index));
}

const SavedDevice* selectedSource(HWND dialog, const DevicePicker& picker)
{
    const auto row = SendDlgItemMessageW(dialog, IDC_REASSIGN_SOURCE, CB_GETCURSEL, 0, 0);
    const auto index = SendDlgItemMessageW(dialog, IDC_REASSIGN_SOURCE, CB_GETITEMDATA, row, 0);
    return index >= 0 && static_cast<std::size_t>(index) < picker.saved.size() ? &picker.saved[index] : nullptr;
}

const DeviceInfo* selectedTarget(HWND dialog, const DevicePicker& picker)
{
    const auto row = SendDlgItemMessageW(dialog, IDC_REASSIGN_TARGET, CB_GETCURSEL, 0, 0);
    const auto index = SendDlgItemMessageW(dialog, IDC_REASSIGN_TARGET, CB_GETITEMDATA, row, 0);
    return index >= 0 && static_cast<std::size_t>(index) < picker.connected.size() ? &picker.connected[index] : nullptr;
}

void refreshDeviceTargets(HWND dialog, const DevicePicker& picker)
{
    const auto source = selectedSource(dialog, picker);
    SendDlgItemMessageW(dialog, IDC_REASSIGN_TARGET, CB_RESETCONTENT, 0, 0);
    if (source) {
        for (std::size_t i = 0; i < picker.connected.size(); ++i) {
            const auto& device = picker.connected[i];
            if (device.id != source->id)
                addDeviceRow(dialog, IDC_REASSIGN_TARGET, deviceLabel(device.id, picker.connected), i);
        }
    }
    EnableWindow(GetDlgItem(dialog, IDC_REASSIGN_TARGET),
                 SendDlgItemMessageW(dialog, IDC_REASSIGN_TARGET, CB_GETCOUNT, 0, 0) > 0);
}

void updateDeviceStatus(HWND dialog, const DevicePicker& picker)
{
    const auto source = selectedSource(dialog, picker);
    const auto target = selectedTarget(dialog, picker);
    const bool supported = source && target && supportsReassignment(source->id, *target, picker.profile, picker.accessories);
    const auto status = supported ? Text::mainReassignReady : target ? Text::mainReassignMissingInputs
        : IsWindowEnabled(GetDlgItem(dialog, IDC_REASSIGN_TARGET)) ? Text::mainReassignChoose : Text::mainReassignConnect;
    SetDlgItemTextW(dialog, IDC_REASSIGN_STATUS, tr(status));
    EnableWindow(GetDlgItem(dialog, IDOK), supported);
}

INT_PTR CALLBACK reassignDeviceProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG) {
        const auto& picker = *reinterpret_cast<DevicePicker*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, tr(Text::mainReassignTitle));
        SetDlgItemTextW(dialog, IDC_REASSIGN_SOURCE_LABEL, tr(Text::mainReassignSource));
        SetDlgItemTextW(dialog, IDC_REASSIGN_TARGET_LABEL, tr(Text::mainReassignTarget));
        SetDlgItemTextW(dialog, IDOK, tr(Text::mainReassignApply));
        SetDlgItemTextW(dialog, IDCANCEL, tr(Text::DlgCancel));
        DialogTheme::attach(dialog, tr(Text::mainReassignHeading), IDC_REASSIGN_STATUS, 0, {IDOK});
        for (std::size_t i = 0; i < picker.saved.size(); ++i) {
            const auto& entry = picker.saved[i];
            addDeviceRow(dialog, IDC_REASSIGN_SOURCE, deviceLabel(entry.id, picker.connected) + L" · " + entry.roles, i);
        }
        SendDlgItemMessageW(dialog, IDC_REASSIGN_SOURCE, CB_SETCURSEL, 0, 0);
        refreshDeviceTargets(dialog, picker);
        updateDeviceStatus(dialog, picker);
        return TRUE;
    }

    if (message != WM_COMMAND)
        return FALSE;
    const auto picker = reinterpret_cast<DevicePicker*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!picker)
        return FALSE;
    switch (LOWORD(wp)) {
    case IDC_REASSIGN_SOURCE:
    case IDC_REASSIGN_TARGET:
        if (HIWORD(wp) == CBN_SELCHANGE) {
            if (LOWORD(wp) == IDC_REASSIGN_SOURCE)
                refreshDeviceTargets(dialog, *picker);
            updateDeviceStatus(dialog, *picker);
        }
        return TRUE;
    case IDOK: {
        const auto source = selectedSource(dialog, *picker);
        const auto target = selectedTarget(dialog, *picker);
        if (source && target && supportsReassignment(source->id, *target, picker->profile, picker->accessories)) {
            picker->choice = DeviceReassignment{source->id, target->id};
            EndDialog(dialog, IDOK);
        }
        return TRUE;
    }
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

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

bool supportsReassignment(std::string_view source, const DeviceInfo& target, const Profile& profile,
                          const AccessoryProfile& accessories)
{
    if (target.id == source)
        return false;
    const auto hasAxis = [&](std::size_t axis) { return axis < target.axes.size() && target.axes[axis]; };
    const std::array axes{&profile.steering, &profile.throttle, &profile.brake};
    if (!std::ranges::all_of(axes, [&](const AxisCalibration* axis) {
            return axis->deviceId != source || hasAxis(axis->axis);
        }))
        return false;
    return std::ranges::all_of(accessories, [&](const AccessoryBinding& binding) {
        if (binding.deviceId != source)
            return true;
        switch (binding.kind) {
        case AccessoryKind::Button: return binding.index < target.buttons.size() && target.buttons.test(binding.index);
        case AccessoryKind::Pov: return binding.index < target.povs.size() && target.povs[binding.index];
        case AccessoryKind::Axis: return hasAxis(binding.index);
        }
        return false;
    });
}

std::optional<DeviceReassignment> chooseDeviceReassignment(HWND owner, const Profile& profile,
    const AccessoryProfile& accessories, const std::vector<DeviceInfo>& connected)
{
    DevicePicker picker{profile, accessories, connected, {}, {}};
    addDeviceRole(picker.saved, profile.steering.deviceId, Text::mainSteering);
    addDeviceRole(picker.saved, profile.throttle.deviceId, Text::mainThrottle);
    addDeviceRole(picker.saved, profile.brake.deviceId, Text::mainBrake);
    std::vector<std::string_view> accessoryDevices;
    for (const auto& binding : accessories) {
        if (std::ranges::find(accessoryDevices, binding.deviceId) == accessoryDevices.end()) {
            accessoryDevices.push_back(binding.deviceId);
            addDeviceRole(picker.saved, binding.deviceId, Text::mainReassignAccessories);
        }
    }
    if (picker.saved.empty()) {
        MessageBoxW(owner, tr(Text::mainReassignNoBindings), tr(Text::mainReassignTitle), MB_OK | MB_ICONINFORMATION);
    } else {
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DEVICE_REASSIGN), owner, reassignDeviceProc,
                        reinterpret_cast<LPARAM>(&picker));
    }
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
