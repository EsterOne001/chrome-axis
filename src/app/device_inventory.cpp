#include "device_inventory.hpp"
#include "input_window.hpp"

#include <algorithm>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace chrome_axis {

namespace {

bool layoutPending(const DeviceInfo& device) noexcept
{
    const auto& pending = device.layoutPending;
    return pending.axes.any() || pending.buttons.any() || pending.povs.any();
}

void retainConfirmedLayout(DeviceInventoryResult& answer, const DeviceInventoryResult& previous)
{
    for (auto& device : answer.devices) {
        if (!layoutPending(device))
            continue;
        const auto known = std::ranges::find(previous.devices, device.id, &DeviceInfo::id);
        if (known == previous.devices.end())
            continue;
        for (std::size_t axis = 0; axis < axisCount; ++axis) {
            if (device.layoutPending.axes[axis]) {
                device.axes[axis] = known->axes[axis];
                device.forceFeedbackAxes[axis] = known->forceFeedbackAxes[axis];
            }
        }
        device.buttons |= device.layoutPending.buttons & known->buttons;
        for (std::size_t pov = 0; pov < povCount; ++pov) {
            if (device.layoutPending.povs[pov])
                device.povs[pov] = known->povs[pov];
        }
    }
}

} // namespace

struct DeviceInventory::State {
    using Clock = std::chrono::steady_clock;

    void run(const Scan& scan)
    {
        std::unique_lock lock(mutex);
        std::uint64_t scanned{};
        Clock::time_point retryAfter{};
        for (;;) {
            if (stopping)
                return;
            if (requested == scanned) {
                const bool active = automaticRetries;
                const auto changed = [&] { return stopping || requested != scanned || active != automaticRetries; };
                if (!active || retryAfter == Clock::time_point{}) {
                    wake.wait(lock, changed);
                    continue;
                }
                if (wake.wait_until(lock, retryAfter, changed))
                    continue;
            }
            const auto revision = requested;
            if (requested != scanned && wake.wait_until(lock, startAfter, [&] { return stopping || requested != revision; }))
                continue;

            lock.unlock();
            auto answer = std::make_shared<DeviceInventoryResult>();
            try {
                *answer = scan();
            } catch (...) {
                // A failure to enumerate must leave the UI able to retry.
            }
            lock.lock();
            if (!stopping && revision == requested) {
                if (result)
                    retainConfirmedLayout(*answer, *result);
                retryAfter = std::ranges::any_of(answer->devices, layoutPending)
                    ? Clock::now() + std::chrono::seconds(1) : Clock::time_point{};
                if (!result || resultRequest != revision || result->available != answer->available ||
                    result->devices != answer->devices) {
                    published = std::max(published + 1, revision);
                    answer->revision = published;
                    resultRequest = revision;
                    result = std::move(answer);
                }
            }
            scanned = revision;
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    Clock::time_point startAfter{};
    std::shared_ptr<const DeviceInventoryResult> result;
    std::uint64_t requested{}, resultRequest{}, published{};
    bool stopping{};
    bool automaticRetries{true};
};

DeviceInventory::DeviceInventory(Scan scan)
    : state_(std::make_shared<State>())
{
    if (!scan) {
        scan = [] {
            const InputWindow window;
            DirectInput input(window.get());
            return DeviceInventoryResult{.devices = input.devices(), .available = input.ready()};
        };
    }
    try {
        // The worker has no window references. A driver call can finish after the UI closes.
        std::thread([state = state_, scan = std::move(scan)] { state->run(scan); }).detach();
    } catch (const std::system_error&) {
        state_->stopping = true;
        state_->result = std::make_shared<DeviceInventoryResult>();
    }
}

DeviceInventory::~DeviceInventory()
{
    {
        std::scoped_lock lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake.notify_one();
}

void DeviceInventory::refresh()
{
    schedule(std::chrono::milliseconds(0));
}

void DeviceInventory::refreshAfterDeviceChange()
{
    schedule(std::chrono::milliseconds(250));
}

void DeviceInventory::setAutomaticRetries(bool active)
{
    {
        std::scoped_lock lock(state_->mutex);
        state_->automaticRetries = active;
    }
    state_->wake.notify_one();
}

void DeviceInventory::schedule(std::chrono::milliseconds delay)
{
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->stopping)
            return;
        ++state_->requested;
        state_->startAfter = State::Clock::now() + delay;
    }
    state_->wake.notify_one();
}

std::shared_ptr<const DeviceInventoryResult> DeviceInventory::current() const
{
    std::scoped_lock lock(state_->mutex);
    const auto& result = state_->result;
    return result && state_->resultRequest == state_->requested ? result : nullptr;
}

} // namespace chrome_axis
