#pragma once

#include "chrome_axis/diagnostics.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/presets.hpp"

#include <algorithm>
#include <cmath>

namespace chrome_axis {

enum class FeedbackEditorStatus { ready, invalid, locked, unreadable, changed };
enum class FeedbackCompatibility { noCalibration, checking, disconnected, unverified, unsupported, supported };

struct FeedbackEditorRead {
    FeedbackEditorStatus status{FeedbackEditorStatus::unreadable};
    std::optional<FeedbackSettings> settings;
    std::optional<ProfileFileRevision> revision;

    bool canSave() const noexcept { return status == FeedbackEditorStatus::ready || status == FeedbackEditorStatus::invalid; }
};

inline bool feedbackCompatibilityPending(FeedbackCompatibility compatibility) noexcept
{
    return compatibility == FeedbackCompatibility::checking || compatibility == FeedbackCompatibility::disconnected ||
        compatibility == FeedbackCompatibility::unverified;
}

inline bool canSaveFeedbackDraft(const FeedbackEditorRead& original, const FeedbackSettings& draft,
                                 FeedbackCompatibility compatibility) noexcept
{
    if (!original.canSave() || (draft.enabled && compatibility != FeedbackCompatibility::supported))
        return false;
    return !feedbackCompatibilityPending(compatibility) || original.status == FeedbackEditorStatus::invalid ||
        (original.settings && original.settings->enabled);
}

inline FeedbackEditorRead readFeedbackForEdit(const std::filesystem::path& path)
{
    const auto before = profileFileRevision(path);
    std::string error;
    DWORD readError{};
    const auto settings = loadFeedbackSettings(path, error, &readError);
    if (!settings && readError) {
        const auto locked = readError == ERROR_SHARING_VIOLATION || readError == ERROR_LOCK_VIOLATION;
        return {.status = locked ? FeedbackEditorStatus::locked : FeedbackEditorStatus::unreadable};
    }
    const auto next = profileFileRevision(path);
    if (!before || !next)
        return {};
    if (before != next)
        return {.status = FeedbackEditorStatus::changed};
    return {settings ? FeedbackEditorStatus::ready : FeedbackEditorStatus::invalid, settings, next};
}

inline PresetSaveResult saveEditedFeedback(const std::filesystem::path& root, const ProfilePreset& preset,
                                           const std::optional<ProfileFileRevision>& calibrationRevision,
                                           const FeedbackEditorRead& original, const FeedbackSettings& settings,
                                           FeedbackEditorStatus& problem)
{
    problem = original.status;
    if (!original.canSave())
        return PresetSaveResult::ioError;
    const std::array checks{ProfileFileCheck{profileFilename, calibrationRevision},
                            ProfileFileCheck{feedbackFilename, original.revision}};
    const auto path = preset.directory / feedbackFilename;
    const auto result = savePresetSettings(root, preset, checks, [&] {
        const auto current = readFeedbackForEdit(path);
        problem = current.status;
        if (!current.canSave())
            return false;
        if (current.revision != original.revision || current.settings != original.settings) {
            problem = FeedbackEditorStatus::changed;
            return false;
        }
        std::string error;
        return saveFeedbackSettings(path, settings, error);
    });
    return problem == FeedbackEditorStatus::changed ? PresetSaveResult::changed : result;
}

inline bool matchesFeedbackMeasurement(const PerformanceReport& capture, const FeedbackSettings& settings,
                                       const Profile& profile, std::string_view name) noexcept
{
    const std::string_view measuredProfile(capture.feedbackProfile.begin(),
        std::ranges::find(capture.feedbackProfile, '\0'));
    return settings.enabled && capture.state == PerformanceState::complete && capture.submittedEffects >= 50 &&
        capture.referenceFeedbackGain == settings.strength && measuredProfile == name &&
        capture.feedbackSetupSignature == feedbackMeasurementSignature(settings, profile) &&
        std::isfinite(capture.peakContinuousForce) && capture.peakContinuousForce >= 0.002f * settings.strength;
}

} // namespace chrome_axis
