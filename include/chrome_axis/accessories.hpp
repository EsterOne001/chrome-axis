#pragma once

#include "chrome_axis/input.hpp"

#include <filesystem>
#include <span>

namespace chrome_axis {

inline constexpr wchar_t accessoryFilename[] = L"accessories.ini";
inline constexpr std::size_t maxAccessoryBindings = 32;

enum class AccessoryKind { Button, Pov, Axis };

struct AccessoryBinding {
    std::string deviceId;
    AccessoryKind kind{};
    std::size_t index{};
    long rest{};
    long engaged{};
    unsigned int key{};

    bool operator==(const AccessoryBinding&) const = default;
};

using AccessoryProfile = std::vector<AccessoryBinding>;

enum class AccessoryStatus { waitingForNeutral, deviceUnavailable, inputUnavailable, armed };

struct AccessoryReadiness {
    std::size_t armed{};
    bool deviceUnavailable{};
    bool inputUnavailable{};
};

bool isAccessoryKey(unsigned int key) noexcept;
std::optional<AccessoryProfile> loadAccessoryProfile(const std::filesystem::path& path, std::string& error);
bool saveAccessoryProfile(const std::filesystem::path& path, const AccessoryProfile& profile, std::string& error);

class AccessoryReader {
public:
    explicit AccessoryReader(const AccessoryProfile& profile);
    std::bitset<256> poll(DirectInput& input);
    void reset() noexcept;
    // All bindings have a usable sample and passed the neutral check.
    bool ready() const noexcept;
    AccessoryReadiness readiness() const noexcept;

    // Samples follow binding order. Bindings on the same device share a sample.
    std::bitset<256> update(std::span<const DeviceSample* const> samples);

private:
    struct BindingState {
        unsigned int neutralPolls{};
        bool active{};
        AccessoryStatus status{AccessoryStatus::waitingForNeutral};
    };

    AccessoryProfile profile_;
    std::array<std::size_t, maxAccessoryBindings> sampleOwners_{};
    std::array<std::bitset<buttonCount>, maxAccessoryBindings> requiredButtons_{};
    std::array<BindingState, maxAccessoryBindings> states_{};
};

} // namespace chrome_axis
