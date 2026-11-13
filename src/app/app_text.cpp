#include "app_text.hpp"

#include "chrome_axis/presets.hpp"

#include <windows.h>

#include <iterator>

namespace chrome_axis {
namespace {

constexpr wchar_t settingsKey[] = L"Software\\Chrome Axis";
constexpr wchar_t languageValue[] = L"Language";
Language choice = Language::automatic;
Language resolved = Language::english;

struct Translation {
    const wchar_t* french;
    const wchar_t* english;
};

constexpr Translation catalog[]{
#define CHROME_AXIS_TEXT(id, french, english) {french, english},
#include "locale_main.inc"
#include "locale_dialogs.inc"
#include "locale_calibration_scope.inc"
#include "locale_calibration_guide.inc"
#include "locale_diagnostics.inc"
#undef CHROME_AXIS_TEXT
};
static_assert(std::size(catalog) == static_cast<std::size_t>(Text::count));

const wchar_t* text(const Translation& translation) noexcept
{
    return resolved == Language::french ? translation.french : translation.english;
}

struct ErrorTranslation {
    std::string_view message;
    Translation translation;
};

// Keep the plugin log messages independent of the app's selected language.
constexpr ErrorTranslation errors[]{
    {"Too many accessory bindings", {L"Trop d’affectations d’accessoires.", L"Too many accessory bindings."}},
    {"Invalid accessory input or keyboard key", {L"Entrée ou touche d’accessoire invalide.", L"Invalid accessory input or key."}},
    {"An accessory keyboard key is assigned more than once", {L"Une touche est affectée à plusieurs accessoires.", L"A key is assigned to more than one accessory."}},
    {"Accessory profile is missing or unreadable", {L"Le profil d’accessoires est introuvable ou illisible.", L"The accessory profile is missing or unreadable."}},
    {"Invalid accessory profile size", {L"Taille du profil d’accessoires invalide.", L"Invalid accessory profile size."}},
    {"Invalid accessory profile entry", {L"Une affectation du profil est invalide.", L"An entry in the accessory profile is invalid."}},
    {"Unsupported or unreadable accessory profile version", {L"Version du profil d’accessoires non prise en charge ou illisible.", L"Unsupported or unreadable accessory profile version."}},
    {"Cannot write the accessory profile. Check the destination and its permissions", {L"Vérifiez le dossier du profil et les droits d’écriture.", L"Check the profile directory and its write permissions."}},
    {"Le profil sélectionné est introuvable.", {L"Le profil sélectionné est introuvable.", L"The selected profile could not be found."}},
    {"Impossible d’enregistrer la sélection dans le dossier Profil.", {L"Impossible d’enregistrer la sélection dans le dossier Profil.", L"Could not save the selection in the profile directory."}},
    {"Saisissez un nom de profil valide (64 caractères maximum).", {L"Saisissez un nom de profil valide (64 caractères maximum).", L"Enter a valid profile name (up to 64 characters)."}},
    {"Sélectionnez un profil existant avant d’en créer une copie.", {L"Sélectionnez un profil existant avant d’en créer une copie.", L"Select an existing profile before making a copy."}},
    {"Impossible de créer le dossier Profils.", {L"Impossible de créer le dossier Profils.", L"Could not create the profiles directory."}},
    {"Un profil porte déjà ce nom.", {L"Un profil porte déjà ce nom.", L"A profile with that name already exists."}},
    {"Impossible de préparer la copie du profil.", {L"Impossible de préparer la copie du profil.", L"Could not prepare a copy of the profile."}},
    {"Impossible de copier les réglages vers le nouveau profil.", {L"Impossible de copier les réglages vers le nouveau profil.", L"Could not copy the settings to the new profile."}},
    {"Le profil a été créé, mais sa sélection n’a pas pu être enregistrée.", {L"Le profil a été créé. Sélectionnez-le dans la liste des profils.", L"The profile was created. Select it in the profile list."}},
};

Language storedLanguage() noexcept
{
    DWORD value{}, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, languageValue, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        if (value == static_cast<DWORD>(Language::french))
            return Language::french;
        if (value == static_cast<DWORD>(Language::english))
            return Language::english;
    }
    return Language::automatic;
}

} // namespace

void initializeLanguage(std::wstring_view commandLine, const std::filesystem::path& profileRoot)
{
    choice = profileLanguageChoice(profileRoot);
    if (commandLine == L"--language=fr")
        choice = Language::french;
    else if (commandLine == L"--language=en")
        choice = Language::english;
    else if (commandLine == L"--language=auto")
        choice = Language::automatic;

    resolved = resolveLanguage(choice);
    SetThreadUILanguage(resolved == Language::french ? MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH)
                                                    : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}

Language languageChoice() noexcept { return choice; }
Language interfaceLanguage() noexcept { return resolved; }

Language profileLanguageChoice(const std::filesystem::path& profileRoot)
{
    if (!profileRoot.empty()) {
        const auto code = readProfileLanguage(profileRoot);
        if (code == "fr")
            return Language::french;
        if (code == "en")
            return Language::english;
        if (code == "auto")
            return Language::automatic;
    }
    return storedLanguage();
}

std::string_view languageCode(Language language) noexcept
{
    if (language == Language::french)
        return "fr";
    if (language == Language::english)
        return "en";
    return "auto";
}

Language resolveLanguage(Language preference) noexcept
{
    if (preference != Language::automatic)
        return preference;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH ? Language::french : Language::english;
}

bool saveLanguageChoice(Language language) noexcept
{
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settingsKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const auto value = static_cast<DWORD>(language);
    const auto result = RegSetValueExW(key, languageValue, 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

const wchar_t* tr(Text id) noexcept
{
    return text(catalog[static_cast<std::size_t>(id)]);
}

std::wstring localizedError(std::string_view error)
{
    for (const auto& entry : errors) {
        if (error == entry.message)
            return text(entry.translation);
    }
    return text({L"Une erreur inattendue s’est produite.", L"An unexpected error occurred."});
}

} // namespace chrome_axis
