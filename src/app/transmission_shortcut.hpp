#pragma once

#include "chrome_axis/configuration.hpp"

namespace chrome_axis {

class TransmissionShortcut {
public:
    void refresh(const std::filesystem::path& directory, std::optional<unsigned int> pluginKey = {})
    {
        saved_ = readTransmissionHotkey(directory);
        std::error_code error;
        if (!saved_ && directory.is_absolute() &&
            !std::filesystem::exists(directory / transmissionHotkeyFilename, error) && !error)
            saved_ = defaultTransmissionHotkey;
        if (saved_)
            lastKnown_ = saved_;
        else if (pluginKey && isTransmissionHotkey(*pluginKey))
            lastKnown_ = pluginKey;
    }

    std::optional<unsigned int> saved() const noexcept { return saved_; }
    std::optional<unsigned int> display() const noexcept { return lastKnown_; }

private:
    std::optional<unsigned int> saved_;
    std::optional<unsigned int> lastKnown_;
};

bool editTransmissionShortcut(HWND owner, const std::filesystem::path& root, TransmissionShortcut& shortcut);

} // namespace chrome_axis
