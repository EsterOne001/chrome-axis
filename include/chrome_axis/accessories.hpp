#pragma once

#include "chrome_axis/input.hpp"

#include <filesystem>
#include <span>

namespace chrome_axis {

inline constexpr wchar_t accessoryFilename[] = L"accessories.ini";
inline constexpr std::string_view progressiveHandbrakePrefix = "# progressive-handbrake=";
inline constexpr std::string_view nativeShifterPrefix = "# native-shifter=";
inline constexpr std::string_view nativeSequentialPrefix = "# native-sequential=";
inline constexpr std::string_view clutchPrefix = "# clutch=";
inline constexpr unsigned int defaultHandbrakeCeilingPercent = 15;
inline constexpr unsigned int maxHandbrakeCeilingPercent = 100;

constexpr bool validHandbrakeCeilingPercent(unsigned int ceiling) noexcept
{
    return ceiling >= 1 && ceiling <= maxHandbrakeCeilingPercent;
}
inline constexpr std::size_t maxAccessoryBindings = 32;
inline constexpr int povDirectionStep = 4500;
inline constexpr int povFullTurn = 36000;

constexpr std::uint16_t quantizePov(std::uint16_t angle) noexcept
{
    if (angle >= povFullTurn)
        return povCentered;
    return static_cast<std::uint16_t>(
        ((angle + povDirectionStep / 2) / povDirectionStep * povDirectionStep) % povFullTurn);
}

enum class AccessoryKind { Button, Pov, Axis };
enum class AccessoryActivation { hold, pulse };
enum class ClutchMode { off, assist, experiment };
enum class ShifterRole { None, Gear1, Gear2, Gear3, Gear4, Gear5, Gear6, Gear7, Gear8, Reverse, Upshift, Downshift };

constexpr unsigned int nativeShifterPositionBit(ShifterRole role) noexcept
{
    if (role == ShifterRole::Reverse)
        return 1u;
    if (role >= ShifterRole::Gear1 && role <= ShifterRole::Gear8)
        return 1u << (static_cast<unsigned int>(role) - static_cast<unsigned int>(ShifterRole::Gear1) + 1);
    return 0;
}

struct AccessoryThresholds {
    unsigned int press{55};
    unsigned int release{35};

    bool operator==(const AccessoryThresholds&) const = default;
};

constexpr bool validAccessoryThresholds(AccessoryThresholds thresholds) noexcept
{
    return thresholds.release >= 1 && thresholds.release < thresholds.press && thresholds.press <= 100;
}

struct AccessoryBinding {
    std::string deviceId;
    AccessoryKind kind{};
    std::size_t index{};
    long rest{};
    long engaged{};
    unsigned int key{};
    AccessoryThresholds thresholds{};
    ShifterRole role{};
    AccessoryActivation activation{AccessoryActivation::hold};
    unsigned int handbrakeCeilingPercent{}; // A nonzero value makes this an analog output.
    bool nativeShifter{};
    unsigned int nativeShifterPositions{}; // Bit 0 is reverse, bits 1 to 8 are forward gears. Zero is unverified.
    bool nativeSequential{};
    std::optional<ClutchMode> clutchMode;

    bool operator==(const AccessoryBinding&) const = default;
};

using AccessoryProfile = std::vector<AccessoryBinding>;

// Position selectors and analog controls are read from their current report.
constexpr bool isDiscreteAccessory(const AccessoryBinding& binding) noexcept
{
    return binding.nativeSequential || binding.key != 0;
}

enum class DiscreteAccessoryLevel { unavailable, pending, neutral, idle, engaged };
DiscreteAccessoryLevel discreteAccessoryLevel(const AccessoryBinding& binding, const DeviceSample& sample,
                                               bool engaged) noexcept;

void removeDisabledDrivingAccessories(AccessoryProfile& profile);

struct Profile;

// Match analog accessories against the driving axes in the same preset.
bool analogAccessoryAxisConflict(const Profile& axes, const AccessoryBinding& binding) noexcept;
// Older profiles can already contain conflicts. Allow editing unrelated settings and resolving conflicts one at a time.
bool introducesAnalogAccessoryAxisConflict(const Profile* previousAxes, std::span<const AccessoryBinding> previousBindings,
                                           const Profile& axes, std::span<const AccessoryBinding> bindings) noexcept;

enum class AccessoryStatus { waitingForNeutral, deviceUnavailable, inputUnavailable, armed, temporarilyUnavailable };

struct AccessoryReadiness {
    std::size_t armed{};
    bool deviceUnavailable{};
    bool inputUnavailable{};
    bool temporarilyUnavailable{};
};

struct AccessoryObservation {
    AccessoryStatus status{AccessoryStatus::waitingForNeutral};
    std::optional<double> travel;
    bool active{};
    std::optional<bool> engaged; // Physical binding state before neutral gating.
    unsigned int key{};
};

struct AccessoryTransmissionSample {
    bool sequentialArmed{};
    unsigned int paddlesPressed{};
    int shiftDelta{}; // -1 or +1 on a new, unambiguous paddle press.
    std::optional<float> clutch; // 0 released, 1 fully depressed, nullopt until armed.
    bool selectorAmbiguous{};
    DeviceReadClock::time_point handbrakeSampledAt{};
    DeviceReadClock::time_point gearSampledAt{};
    DeviceReadClock::time_point sequentialSampledAt{};
    DeviceReadClock::time_point clutchSampledAt{};
    DeviceReadClock::time_point shiftSampledAt{};
    DeviceReadClock::time_point oppositePaddleSampledAt{};
};

using AccessoryKeyTimes = std::array<DeviceReadClock::time_point, 256>;

bool isAccessoryKey(unsigned int key) noexcept;
bool nativeShifterVerified(const AccessoryProfile& profile) noexcept;
std::optional<AccessoryProfile> loadAccessoryProfile(const std::filesystem::path& path, std::string& error);
bool saveAccessoryProfile(const std::filesystem::path& path, const AccessoryProfile& profile, std::string& error);
bool sameAccessoryInputProfile(const std::optional<AccessoryProfile>& first,
                               const std::optional<AccessoryProfile>& second);

class AccessoryReader {
public:
    explicit AccessoryReader(const AccessoryProfile& profile);
    // Returns held keys and optional pulse, handbrake and transmission outputs.
    // Native gears are -1 neutral, 0 reverse and 1–8 forward; nullopt is unavailable.
    std::bitset<256> poll(DeviceSampleSource& input, std::span<AccessoryObservation> observations = {},
                         std::bitset<256>* pulses = nullptr,
                         std::optional<float>* progressiveHandbrake = nullptr,
                         std::optional<int>* nativeGear = nullptr,
                         AccessoryTransmissionSample* transmission = nullptr,
                         AccessoryKeyTimes* keyTimes = nullptr, bool replayDiscrete = false);
    void reset() noexcept;
    void resetKeyboardBindings() noexcept;
    void resetDiscrete(std::string_view deviceId) noexcept;
    void resetSequential() noexcept;
    std::bitset<256> armedKeyboardBindings() const noexcept;
    // All bindings have a usable sample and passed the neutral check.
    bool ready() const noexcept;
    AccessoryReadiness readiness(bool nativeSelectorUsable = true) const noexcept;

    // Samples follow binding order. Bindings on the same device share a sample.
    std::bitset<256> update(std::span<const DeviceSample* const> samples, std::span<const DeviceReadStatus> statuses = {},
                            std::span<AccessoryObservation> observations = {}, std::bitset<256>* pulses = nullptr,
                            std::optional<float>* progressiveHandbrake = nullptr,
                            std::optional<int>* nativeGear = nullptr,
                            AccessoryTransmissionSample* transmission = nullptr,
                            AccessoryKeyTimes* keyTimes = nullptr, bool replayDiscrete = false);

private:
    struct BindingState {
        unsigned int neutralPolls{};
        bool active{};
        bool waitForRelease{};
        AccessoryStatus status{AccessoryStatus::waitingForNeutral};
        std::uint64_t sequence{};
    };

    struct NativeShifterState {
        unsigned int neutralPolls{};
        unsigned int candidatePolls{};
        std::optional<int> candidate;
        std::optional<int> stable;
    };

    static float advance(const AccessoryBinding& binding, BindingState& state, const DeviceSample* sample,
                         DeviceReadStatus status, AccessoryObservation* observation, bool* neutral);

    AccessoryProfile profile_;
    bool nativeShifterVerified_{};
    NativeShifterState nativeShifter_;
    unsigned int sequentialNeutralPolls_{};
    bool sequentialWaitForRelease_{};
    std::array<std::size_t, maxAccessoryBindings> sampleOwners_{};
    std::array<std::bitset<buttonCount>, maxAccessoryBindings> requiredButtons_{};
    std::array<BindingState, maxAccessoryBindings> states_{};
    std::array<std::uint64_t, maxAccessoryBindings> selectorSequences_{};
};

} // namespace chrome_axis
