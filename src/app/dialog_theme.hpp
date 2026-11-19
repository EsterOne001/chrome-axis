#pragma once

#include <windows.h>

#include <initializer_list>

namespace chrome_axis {

enum class DialogDecoration { header, content, scrollable };

class DialogTheme {
public:
    static void attach(HWND dialog, const wchar_t* subtitle, int statusId, int listId,
                       std::initializer_list<int> primaryButtons,
                       DialogDecoration decoration);
    static void setPrimary(HWND dialog, int controlId, bool primary);
};

} // namespace chrome_axis
