#include "hud_color_storage.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

namespace {

constexpr std::string_view original = R"({"presets":{"night":{"transmission":[255,0,0]}}})";
constexpr std::string_view edited = R"({"presets":{"night":{"transmission":[255,0,0],"border":[0,255,126]}}})";
constexpr std::string_view external = R"({"presets":{"Préréglage":{"border":[15,10,20]}}})";

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool check(bool condition, const char* description) {
    if (!condition)
        std::cerr << description << '\n';
    return condition;
}

} // namespace

int main() {
    using chrome_axis::commitHudColors;
    const auto game = chrome_axis::detail::stagedSettingsPath(std::filesystem::temp_directory_path()
                                                              / L"chrome-axis-hud-test");
    if (!std::filesystem::create_directory(game))
        return 1;
    struct Fixture {
        const std::filesystem::path& root;
        ~Fixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } fixture{game};
    const auto directory = game / L"r6" / L"storages" / L"ChromeAxisHUD";
    const auto colors = directory / L"colors.json";

    bool ok = check(commitHudColors(game, original, {}, false) && read(colors) == original,
                    "A first save must create the documented color file.");
    ok &= check(!commitHudColors(game, edited, {}, false) && read(colors) == original,
                "A file appearing since the initial read must not be overwritten.");

    {
        const auto lock = chrome_axis::detail::hudColorHandle(CreateFileW(colors.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        ok &= check(static_cast<bool>(lock), "Could not simulate a reader that prevents replacement.");
        ok &= check(!commitHudColors(game, edited, original, true) && read(colors) == original,
                    "A locked file must retain its original data.");
    }
    ok &= check(commitHudColors(game, edited, original, true) && read(colors) == edited,
                "The pending colors must replace the original file after the lock is released.");

    ok &= check(commitHudColors(game, external, edited, true) && read(colors) == external,
                "An unrelated preset with a UTF-8 name must remain byte-for-byte intact when written.");
    ok &= check(!commitHudColors(game, edited, original, true) && read(colors) == external,
                "A stale merge must leave the more recent data untouched.");

    constexpr auto prettyWindows = "{\r\n  \"presets\": {}\r\n}\r\n";
    constexpr auto prettyRead = "{\n  \"presets\": {}\n}\n";
    ok &= check(commitHudColors(game, prettyWindows, external, true)
                    && commitHudColors(game, edited, prettyRead, true) && read(colors) == edited,
                "The compare step must accept Windows line endings translated by RedFileSystem.");
    ok &= check(std::distance(std::filesystem::directory_iterator(directory),
                             std::filesystem::directory_iterator()) == 1,
                "A staged file must be cleaned up after success and after a rejected replacement.");
    return ok ? 0 : 1;
}
