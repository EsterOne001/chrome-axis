#pragma once

#include "chrome_axis/controls.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace chrome_axis {

class SimulationPublisher {
public:
    explicit SimulationPublisher(const std::filesystem::path& gameRoot);
    ~SimulationPublisher();
    SimulationPublisher(const SimulationPublisher&) = delete;
    SimulationPublisher& operator=(const SimulationPublisher&) = delete;

    bool enable() noexcept;
    void disable() noexcept;
    void heartbeat() noexcept;
    bool enabled() const noexcept;

private:
    std::wstring channel_;
    std::wstring ownerName_;
    HANDLE owner_{};
    HANDLE mapping_{};
    LONG64* frame_{};
    std::uint64_t generation_{};
};

enum class SimulationSession { inactive, active, restarted };

class SimulationReceiver {
public:
    explicit SimulationReceiver(const std::filesystem::path& gameRoot);
    ~SimulationReceiver();
    SimulationReceiver(const SimulationReceiver&) = delete;
    SimulationReceiver& operator=(const SimulationReceiver&) = delete;

    SimulationSession session() noexcept;

private:
    std::wstring channel_;
    HANDLE mapping_{};
    const LONG64* frame_{};
    std::uint64_t generation_{};
    std::uint64_t retryAt_{};
};

struct SimulationKeys {
    bool left{};
    bool right{};
    bool throttle{};
    bool brake{};
};

// Call only while the application or game owns the foreground window.
SimulationKeys simulationKeys() noexcept;

class Simulator {
public:
    Controls update(SimulationKeys keys, float seconds) noexcept;
    void reset() noexcept;

private:
    Controls controls_{};
    bool armed_{};
};

} // namespace chrome_axis
