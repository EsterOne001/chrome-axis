#pragma once

#include "chrome_axis/feedback.hpp"
#include "chrome_axis/presets.hpp"

#include <chrono>

namespace chrome_axis {

struct UsbProfileRefresh {
    std::optional<ProfilePreset> selected;
    std::optional<FeedbackSettings> disabledFeedback;
    std::optional<DWORD> feedbackReadError;
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
            // The prior profile remains live. Check whether it can still safely generate force.
            std::string error;
            DWORD readError{};
            auto settings = loadFeedbackSettings(directory / feedbackFilename, error, &readError);
            if (!settings)
                result.feedbackReadError = readError;
            else if (!settings->enabled)
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
