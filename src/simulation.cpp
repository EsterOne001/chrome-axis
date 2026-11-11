#include "chrome_axis/simulation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace chrome_axis {
namespace {

// Keep the generation and heartbeat in one aligned word so a read-only receiver
// can observe them together. A zero stamp represents an explicitly stopped app.
constexpr unsigned int timestampBits = 48;
constexpr auto timestampMask = (std::uint64_t{1} << timestampBits) - 1;
constexpr auto maxGeneration = (std::uint64_t{1} << (64 - timestampBits)) - 1;
constexpr std::uint64_t heartbeatLifetime = 350;
constexpr std::uint64_t openRetry = 100;

std::uint64_t readFrame(const LONG64* frame) noexcept
{
    return std::bit_cast<std::uint64_t>(ReadAcquire64(frame));
}

void publishFrame(LONG64* frame, std::uint64_t value) noexcept
{
    WriteRelease64(frame, std::bit_cast<LONG64>(value));
}

bool keyDown(int key) noexcept
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

float approach(float value, float target, float maxChange) noexcept
{
    return value + std::clamp(target - value, -maxChange, maxChange);
}

} // namespace

SimulationPublisher::SimulationPublisher(std::wstring channel)
    : channel_(std::move(channel)), ownerName_(channel_ + L".publisher")
{
}

SimulationPublisher::~SimulationPublisher()
{
    disable();
}

bool SimulationPublisher::enable() noexcept
{
    if (enabled())
        return true;

    // Own the lifetime of this name, not the mutex lock. That also rejects a
    // second publisher on the same thread and releases ownership on process exit.
    owner_ = CreateMutexW(nullptr, FALSE, ownerName_.c_str());
    if (!owner_ || GetLastError() == ERROR_ALREADY_EXISTS) {
        disable();
        return false;
    }
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  sizeof(LONG64), channel_.c_str());
    if (mapping_)
        frame_ = static_cast<LONG64*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(LONG64)));
    if (!frame_) {
        disable();
        return false;
    }
    generation_ = (readFrame(frame_) >> timestampBits) % maxGeneration + 1;
    heartbeat();
    return true;
}

void SimulationPublisher::disable() noexcept
{
    if (frame_) {
        publishFrame(frame_, generation_ << timestampBits);
        UnmapViewOfFile(std::exchange(frame_, nullptr));
    }
    if (mapping_)
        CloseHandle(std::exchange(mapping_, nullptr));
    if (owner_)
        CloseHandle(std::exchange(owner_, nullptr));
}

void SimulationPublisher::heartbeat() noexcept
{
    if (frame_) {
        const auto stamp = (GetTickCount64() + 1) & timestampMask;
        publishFrame(frame_, (generation_ << timestampBits) | stamp);
    }
}

bool SimulationPublisher::enabled() const noexcept
{
    return frame_ != nullptr;
}

SimulationReceiver::SimulationReceiver(std::wstring channel) : channel_(std::move(channel))
{
}

SimulationReceiver::~SimulationReceiver()
{
    if (frame_)
        UnmapViewOfFile(frame_);
    if (mapping_)
        CloseHandle(mapping_);
}

SimulationSession SimulationReceiver::session() noexcept
{
    const auto now = GetTickCount64();
    if (!frame_) {
        if (now < retryAt_)
            return SimulationSession::inactive;
        retryAt_ = now + openRetry;
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, channel_.c_str());
        if (!mapping_)
            return SimulationSession::inactive;
        frame_ = static_cast<const LONG64*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(LONG64)));
        if (!frame_) {
            CloseHandle(std::exchange(mapping_, nullptr));
            return SimulationSession::inactive;
        }
    }

    const auto value = readFrame(frame_);
    const auto stamp = value & timestampMask;
    const auto generation = value >> timestampBits;
    // Recheck the clock after the read. The publisher can tick between the
    // mapping lookup above and the read, giving the frame a newer timestamp.
    const auto age = (GetTickCount64() + 1 - stamp) & timestampMask;
    if (generation == 0 || stamp == 0 || age > heartbeatLifetime) {
        generation_ = 0;
        return SimulationSession::inactive;
    }
    const bool changed = generation_ != 0 && generation_ != generation;
    generation_ = generation;
    return changed ? SimulationSession::restarted : SimulationSession::active;
}

SimulationKeys simulationKeys() noexcept
{
    return {
        keyDown(VK_NUMPAD4) || keyDown(VK_F6),
        keyDown(VK_NUMPAD6) || keyDown(VK_F7),
        keyDown(VK_NUMPAD8) || keyDown(VK_F8),
        keyDown(VK_NUMPAD2) || keyDown(VK_F10)
    };
}

Controls Simulator::update(SimulationKeys keys, float seconds) noexcept
{
    if (!armed_) {
        armed_ = !keys.left && !keys.right && !keys.throttle && !keys.brake;
        return {};
    }
    const float dt = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, 0.1f) : 0.0f;
    const float target = keys.left == keys.right ? 0.0f : (keys.left ? -1.0f : 1.0f);
    controls_.steering = approach(controls_.steering, target, dt * (target == 0.0f ? 5.0f : 3.0f));
    controls_.throttle = keys.throttle ? approach(controls_.throttle, 1.0f, dt * 3.0f) : 0.0f;
    controls_.brake = keys.brake ? approach(controls_.brake, 1.0f, dt * 4.0f) : 0.0f;
    return controls_;
}

void Simulator::reset() noexcept
{
    controls_ = {};
    armed_ = false;
}

} // namespace chrome_axis
