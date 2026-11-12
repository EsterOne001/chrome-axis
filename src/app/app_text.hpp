#pragma once

#include <string>
#include <string_view>

namespace chrome_axis {

enum class Language { automatic, french, english };

enum class Text {
#define CHROME_AXIS_TEXT(id, french, english) id,
#include "locale_main.inc"
#include "locale_dialogs.inc"
#undef CHROME_AXIS_TEXT
    count
};

// Resolve the UI language once, before creating any window or dialog.
void initializeLanguage(std::wstring_view commandLine = {}) noexcept;
Language languageChoice() noexcept;
Language interfaceLanguage() noexcept;
Language resolveLanguage(Language preference) noexcept;
bool saveLanguageChoice(Language language) noexcept;

const wchar_t* tr(Text id) noexcept;
std::wstring localizedError(std::string_view error);

} // namespace chrome_axis
