#pragma once

#define IDD_PROFILES 206
#define IDC_PROFILE_LIST 2601
#define IDC_PROFILE_NAME 2602
#define IDC_PROFILE_COPY 2603
#define IDC_PROFILE_LIST_LABEL 2604
#define IDC_PROFILE_NAME_LABEL 2605
#define IDD_DEVICE_REASSIGN 210
#define IDC_REASSIGN_SOURCE 2610
#define IDC_REASSIGN_TARGET 2611
#define IDC_REASSIGN_SOURCE_LABEL 2612
#define IDC_REASSIGN_TARGET_LABEL 2613
#define IDC_REASSIGN_STATUS 2614

#ifndef RC_INVOKED
#include "chrome_axis/accessories.hpp"
#include "chrome_axis/controls.hpp"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chrome_axis {

struct ProfileChoice {
    std::wstring name;
    bool copy{};
};

struct DeviceReassignment {
    std::string source;
    std::string target;
};

std::optional<ProfileChoice> chooseProfile(HWND owner, const std::vector<std::wstring>& names,
                                           std::wstring_view active);
std::optional<DeviceReassignment> chooseDeviceReassignment(HWND owner, const Profile& profile,
    const AccessoryProfile& accessories, const std::vector<DeviceInfo>& connected);
bool supportsReassignment(std::string_view source, const DeviceInfo& target, const Profile& profile,
                          const AccessoryProfile& accessories);
HRESULT chooseProfileFolder(HWND owner, const wchar_t* title, const std::filesystem::path& initial,
                            std::filesystem::path& result);

} // namespace chrome_axis
#endif
