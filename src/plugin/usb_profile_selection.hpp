#pragma once

#include "chrome_axis/presets.hpp"

#include <chrono>

namespace chrome_axis {

class UsbProfileSelection {
public:
    using Clock = std::chrono::steady_clock;

    // An empty optional defers a locked selection. An empty directory clears it.
    std::optional<ProfilePreset> refresh(const std::filesystem::path& root, bool requested,
                                        bool hasCurrent, Clock::time_point now)
    {
        if (!requested) {
            lockedSince_.reset();
            return ProfilePreset{};
        }

        auto selection = readActivePreset(root);
        if (selection) {
            lockedSince_.reset();
            return selection;
        }
        if (!hasCurrent)
            return ProfilePreset{};

        if (!lockedSince_)
            lockedSince_ = now;
        if (now - *lockedSince_ < std::chrono::seconds{1})
            return std::nullopt;
        return ProfilePreset{};
    }

private:
    std::optional<Clock::time_point> lockedSince_;
};

} // namespace chrome_axis
