#include "device_inventory.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace chrome_axis {

struct DeviceInventory::State {
    using Clock = std::chrono::steady_clock;

    void run(const Scan& scan)
    {
        std::unique_lock lock(mutex);
        std::uint64_t scanned{};
        for (;;) {
            wake.wait(lock, [&] { return stopping || requested != scanned; });
            if (stopping)
                return;
            const auto revision = requested;
            if (wake.wait_until(lock, startAfter, [&] { return stopping || requested != revision; }))
                continue;
            lock.unlock();
            auto answer = std::make_shared<DeviceInventoryResult>();
            try {
                *answer = scan();
            } catch (...) {
                // A failure to enumerate must leave the UI able to retry.
            }
            answer->revision = revision;
            lock.lock();
            if (!stopping && revision == requested)
                result = std::move(answer);
            scanned = revision;
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    Clock::time_point startAfter{};
    std::shared_ptr<const DeviceInventoryResult> result;
    std::uint64_t requested{};
    bool stopping{};
};

DeviceInventory::DeviceInventory(Scan scan)
    : state_(std::make_shared<State>())
{
    if (!scan) {
        scan = [] {
            DirectInput input(nullptr);
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
    return result && result->revision == state_->requested ? result : nullptr;
}

} // namespace chrome_axis
