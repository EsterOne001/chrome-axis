#pragma once

#include "chrome_axis/accessories.hpp"
#include "chrome_axis/controls.hpp"
#include "chrome_axis/feedback.hpp"
#include "chrome_axis/presets.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <utility>

namespace chrome_axis {

inline bool missingEditorFile(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

inline bool lockedEditorFile(DWORD error)
{
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

struct EditorCalibration {
    std::optional<Profile> profile;
    std::optional<ProfileFileRevision> revision;
    bool locked{};
};

// A revision alone is insufficient when Windows permits reading metadata but denies access to the contents.
inline EditorCalibration readEditorCalibration(const std::filesystem::path& path)
{
    const auto revision = profileFileRevision(path);
    if (!revision)
        return {};

    DWORD code{};
    std::string error;
    auto profile = loadProfile(path, error, &code);
    const bool consistent = revision->present ? profile.has_value() : missingEditorFile(code);
    if (!consistent || profileFileRevision(path) != revision)
        return {.locked = lockedEditorFile(code)};
    return {std::move(profile), revision};
}

inline std::optional<AccessoryProfile> loadEditorAccessories(const std::filesystem::path& path, std::string& error,
                                                              DWORD* readError = nullptr)
{
    DWORD code{};
    auto bindings = loadAccessoryProfile(path, error, &code);
    if (readError)
        *readError = code;
    if (!bindings && missingEditorFile(code)) {
        error.clear();
        return AccessoryProfile{};
    }
    return bindings;
}

enum class UsbActivationStatus { ready, readyWithUnreadableAccessories, commandsMissing, calibrationLocked,
                                 accessoriesLocked, accessoriesUnreadable, axisConflict };

// Called with the selected profile locked against concurrent edits and profile changes.
inline UsbActivationStatus readUsbActivation(const std::filesystem::path& directory)
{
    std::string error;
    DWORD calibrationError{}, accessoryError{};
    const auto axes = loadProfile(directory / profileFilename, error, &calibrationError);
    const auto bindings = loadEditorAccessories(directory / accessoryFilename, error, &accessoryError);
    // Only this new activation checks both files. The plugin's existing request can recover from a brief lock.
    if (axes) {
        if (lockedEditorFile(accessoryError))
            return UsbActivationStatus::accessoriesLocked;
        if (!bindings)
            return UsbActivationStatus::readyWithUnreadableAccessories;
        return hasAnalogAccessoryAxisConflict(*axes, *bindings) ? UsbActivationStatus::axisConflict
                                                               : UsbActivationStatus::ready;
    }
    if (bindings && std::ranges::any_of(*bindings, isEnabledDrivingAccessory))
        return UsbActivationStatus::ready;
    if (lockedEditorFile(calibrationError))
        return UsbActivationStatus::calibrationLocked;
    if (bindings)
        return UsbActivationStatus::commandsMissing;
    return lockedEditorFile(accessoryError) ? UsbActivationStatus::accessoriesLocked : UsbActivationStatus::accessoriesUnreadable;
}

enum class AccessoryPreviewStatus { available, locked, unreadable };

// The displayed copy survives a temporary lock on the same file. Editors still read from disk.
class AccessoryProfilePreview {
public:
    bool refresh(const std::filesystem::path& path)
    {
        const auto previousStatus = status_;
        const bool profileChanged = path_ != path;
        if (profileChanged) {
            path_ = path;
            bindings_.clear();
        }

        DWORD code{};
        std::string error;
        auto updated = path_.empty() ? std::optional{AccessoryProfile{}} : loadEditorAccessories(path_, error, &code);
        if (lockedEditorFile(code)) {
            status_ = AccessoryPreviewStatus::locked;
            return profileChanged || previousStatus != status_;
        }

        status_ = updated ? AccessoryPreviewStatus::available : AccessoryPreviewStatus::unreadable;
        auto bindings = std::move(updated).value_or(AccessoryProfile{});
        const bool changed = profileChanged || bindings != bindings_ || previousStatus != status_;
        bindings_ = std::move(bindings);
        return changed;
    }

    AccessoryPreviewStatus status() const noexcept { return status_; }
    const AccessoryProfile& bindings() const noexcept { return bindings_; }

private:
    std::filesystem::path path_;
    AccessoryProfile bindings_;
    AccessoryPreviewStatus status_{};
};

enum class ProfileEditorError { none, calibrationUnreadable, accessoriesUnreadable, axisConflict,
                                dedicatedAccessoryAxisConflict, feedbackUnreadable, feedbackWriteFailed, createDirectoryFailed };

struct ProfileEditorSave {
    PresetSaveResult result{PresetSaveResult::ioError};
    ProfileEditorError reason{ProfileEditorError::none};
    std::string details;
    bool feedbackDisabled{};
    std::optional<ProfileFileRevision> revision;
};

inline const AxisCalibration* steeringActuator(const std::optional<Profile>& profile)
{
    return profile && !profile->steering.deviceId.empty() ? &profile->steering : nullptr;
}

inline bool sameSteeringActuator(const std::optional<Profile>& previous, const std::optional<Profile>& next)
{
    const auto* oldSteering = steeringActuator(previous);
    const auto* newSteering = steeringActuator(next);
    if (!oldSteering || !newSteering)
        return oldSteering == newSteering;
    return oldSteering->deviceId == newSteering->deviceId && oldSteering->axis == newSteering->axis;
}

inline bool disableOldFeedback(const std::filesystem::path& path, ProfileEditorSave& save)
{
    auto settings = loadFeedbackSettings(path, save.details);
    if (!settings) {
        save.reason = ProfileEditorError::feedbackUnreadable;
        return false;
    }
    if (!settings->enabled)
        return true;
    settings->enabled = false;
    save.feedbackDisabled = saveFeedbackSettings(path, *settings, save.details);
    if (!save.feedbackDisabled)
        save.reason = ProfileEditorError::feedbackWriteFailed;
    return save.feedbackDisabled;
}

inline ProfileEditorSave saveEditorCalibration(const std::filesystem::path& root, const ProfilePreset& preset,
                                                const std::optional<ProfileFileRevision>& expectedRevision,
                                                const std::optional<Profile>& updated)
{
    ProfileEditorSave save;
    const auto path = preset.directory / profileFilename;
    const std::array checks{ProfileFileCheck{profileFilename, expectedRevision}};
    save.result = savePresetSettings(root, preset, checks, [&] {
        const auto previous = readEditorCalibration(path);
        if (!previous.revision || previous.revision != expectedRevision) {
            save.reason = ProfileEditorError::calibrationUnreadable;
            return false;
        }
        if (updated) {
            const auto accessories = loadEditorAccessories(preset.directory / accessoryFilename, save.details);
            if (!accessories) {
                save.reason = ProfileEditorError::accessoriesUnreadable;
                return false;
            }
            if (introducesAnalogAccessoryAxisConflict(previous.profile ? &*previous.profile : nullptr,
                                                       *accessories, *updated, *accessories)) {
                save.reason = ProfileEditorError::axisConflict;
                return false;
            }
        }
        if (!sameSteeringActuator(previous.profile, updated) &&
            !disableOldFeedback(preset.directory / feedbackFilename, save))
            return false;
        if (updated) {
            std::error_code error;
            std::filesystem::create_directories(preset.directory, error);
            if (error || !saveProfile(path, *updated, save.details))
                return false;
        } else if (!DeleteFileW(path.c_str()) && !missingEditorFile(GetLastError())) {
            return false;
        }
        save.revision = profileFileRevision(path);
        return true;
    });
    return save;
}

inline ProfileEditorSave saveEditorAccessories(const std::filesystem::path& root, const ProfilePreset& preset,
                                                const std::optional<ProfileFileRevision>& expectedRevision,
                                                const AccessoryProfile& updated)
{
    ProfileEditorSave save;
    const auto path = preset.directory / accessoryFilename;
    const std::array checks{ProfileFileCheck{accessoryFilename, expectedRevision}};
    save.result = savePresetSettings(root, preset, checks, [&] {
        const auto axes = readEditorCalibration(preset.directory / profileFilename);
        if (!axes.revision) {
            save.reason = ProfileEditorError::calibrationUnreadable;
            return false;
        }
        const bool dedicatedAxisConflict = hasDedicatedAccessoryAxisConflict(updated);
        if (axes.profile || dedicatedAxisConflict) {
            const auto previous = expectedRevision->present ? loadAccessoryProfile(path, save.details)
                                                            : std::optional{AccessoryProfile{}};
            if (!previous) {
                save.reason = ProfileEditorError::accessoriesUnreadable;
                return false;
            }
            if (dedicatedAxisConflict && !hasDedicatedAccessoryAxisConflict(*previous)) {
                save.reason = ProfileEditorError::dedicatedAccessoryAxisConflict;
                return false;
            }
            if (axes.profile && introducesAnalogAccessoryAxisConflict(&*axes.profile, *previous, *axes.profile, updated)) {
                save.reason = ProfileEditorError::axisConflict;
                return false;
            }
        }
        std::error_code error;
        std::filesystem::create_directories(preset.directory, error);
        if (error) {
            save.reason = ProfileEditorError::createDirectoryFailed;
            return false;
        }
        return saveAccessoryProfile(path, updated, save.details);
    });
    return save;
}

} // namespace chrome_axis
