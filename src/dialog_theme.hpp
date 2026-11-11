#pragma once

#include <windows.h>

#include <initializer_list>

namespace chrome_axis {

class DialogTheme {
public:
    static void attach(HWND dialog, const wchar_t* subtitle, int statusId, int listId,
                       std::initializer_list<int> primaryButtons);
};

} // namespace chrome_axis
