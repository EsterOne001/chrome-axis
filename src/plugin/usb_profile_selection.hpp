#pragma once

#include "chrome_axis/feedback.hpp"
#include "chrome_axis/presets.hpp"

#include <chrono>

namespace chrome_axis {

struct UsbProfileRefresh {
    std::optional<ProfilePreset> selected;
    std::optional<FeedbackSettings> disabledFeedback;
};

class UsbProfileSelection {
public:
    using Clock = std::chrono::steady_clock;

    UsbProfileRefresh refresh(const std::filesystem::path& root, bool requested,
                              const std::filesystem::path& directory, bool feedbackEnabled,
                              Clock::time_point now)
    {
        UsbProfileRefresh result{.selected = refresh(root, requested, !directory.empty(), now)};
        if (!result.selected && feedbackEnabled) {
            // The prior profile remains live. Apply a saved stop even while the selector is unreadable.
            std::string error;
            auto settings = loadFeedbackSettings(directory / feedbackFilename, error);
            if (settings && !settings->enabled)
                result.disabledFeedback = settings;
        }
        return result;
    }

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
