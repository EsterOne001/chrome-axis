#pragma once

#define IDD_PROFILES 206
#define IDC_PROFILE_LIST 2601
#define IDC_PROFILE_NAME 2602
#define IDC_PROFILE_COPY 2603

#ifndef RC_INVOKED
#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chrome_axis {

struct ProfileChoice {
    std::wstring name;
    bool copy{};
};

std::optional<ProfileChoice> chooseProfile(HWND owner, const std::vector<std::wstring>& names,
                                           std::wstring_view active);

} // namespace chrome_axis
#endif
