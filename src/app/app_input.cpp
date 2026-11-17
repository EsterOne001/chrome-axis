#include "app_input.hpp"
#include "input_window.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace chrome_axis {
namespace {

using Clock = DeviceReadClock;
using Buttons = std::bitset<buttonCount>;
constexpr auto sampleLifetime = std::chrono::milliseconds(120);
constexpr auto initializationRetry = std::chrono::milliseconds(250);

} // namespace

struct AppInput::State {
    struct Report {
        CaptureDeviceRead reading;
        Buttons buttons;
        std::uint64_t number;
    };

    struct Device {
        explicit Device(std::string deviceId) : id(std::move(deviceId)) {}
        ~Device() { if (wake) CloseHandle(wake); }

        void run(State& state)
        {
            const InputWindow window;
            std::optional<DirectInput> driver;
            Clock::time_point nextInitialization{};
            std::unique_lock lock(mutex);
            while (!stopping) {
                if (std::exchange(closeInput, false)) {
                    lock.unlock();
                    driver.reset();
                    nextInitialization = {};
                    lock.lock();
                    continue;
                }
                if (!std::exchange(queued, false)) {
                    lock.unlock();
                    if (MsgWaitForMultipleObjectsEx(1, &wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                        return;
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                        DispatchMessageW(&message);
                    lock.lock();
                    continue;
                }

                const auto buttons = std::exchange(requested, {});
                const auto generation = revision;
                lock.unlock();

                const auto startedAt = Clock::now();
                CaptureDeviceRead reading;
                if (state.read) {
                    reading = state.read(id, buttons);
                } else if (window.get()) {
                    if (!driver && startedAt >= nextInitialization) {
                        driver.emplace(window.get());
                        if (!driver->ready()) {
                            driver.reset();
                            nextInitialization = Clock::now() + initializationRetry;
                        }
                    }
                    if (driver) {
                        driver->beginPoll();
                        reading.sample = driver->sample(id, buttons, &reading.status);
                    }
                }

                lock.lock();
                if (stopping || generation != revision)
                    continue;
                const auto number = state.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reading.sample) {
                    if (reading.sample->sampledAt == Clock::time_point{})
                        reading.sample->sampledAt = startedAt;
                    if (Clock::now() - reading.sample->sampledAt > sampleLifetime) {
                        reading.sample.reset();
                        reading.status = DeviceReadStatus::unavailable;
                    } else {
                        reading.sample->sequence = number;
                    }
                }
                interruption |= reading.status == DeviceReadStatus::resumed ||
                    (observedSample && reading.status == DeviceReadStatus::unavailable);
                observedSample |= reading.sample.has_value();
                report = Report{std::move(reading), buttons, number};
            }
        }

        CaptureDeviceRead request(const Buttons& buttons, bool capture)
        {
            std::scoped_lock lock(mutex);
            if (stopping)
                return {};
            requested |= buttons;
            if (!std::exchange(queued, true))
                SetEvent(wake);

            constexpr CaptureDeviceRead pending{.sample = {}, .status = DeviceReadStatus::pending, .updated = false};
            if (!report || (buttons & ~report->buttons).any())
                return pending;

            auto reading = report->reading;
            const bool expired = reading.sample && Clock::now() - reading.sample->sampledAt > sampleLifetime;
            if (expired) {
                reading.sample.reset();
                reading.status = DeviceReadStatus::unavailable;
                interruption = true;
            }
            if (capture) {
                const bool updated = delivered != report->number || deliveredExpiry != expired;
                delivered = report->number;
                deliveredExpiry = expired;
                if (!updated)
                    return pending;
            }
            if (reading.sample)
                reading.status = std::exchange(interruption, false) ? DeviceReadStatus::resumed : DeviceReadStatus::available;
            return reading;
        }

        void reset(bool releaseDevice)
        {
            std::scoped_lock lock(mutex);
            ++revision;
            report.reset();
            requested.reset();
            delivered = 0;
            queued = deliveredExpiry = interruption = observedSample = false;
            closeInput |= releaseDevice;
            if (wake)
                SetEvent(wake);
        }

        void stop()
        {
            std::scoped_lock lock(mutex);
            stopping = true;
            if (wake)
                SetEvent(wake);
        }

        const std::string id;
        const HANDLE wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        std::mutex mutex;
        std::optional<Report> report;
        Buttons requested;
        std::uint64_t revision{}, delivered{};
        bool queued{}, deliveredExpiry{}, interruption{}, observedSample{}, stopping{}, closeInput{};
    };

    explicit State(Read read) : read(std::move(read)) {}

    const Read read;
    std::atomic_uint64_t sequence{};
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Device>> devices;
};

AppInput::AppInput(Read read) : state_(std::make_shared<State>(std::move(read))) {}

AppInput::~AppInput()
{
    std::scoped_lock lock(state_->mutex);
    for (const auto& entry : state_->devices)
        entry.second->stop();
    state_->devices.clear();
}

std::optional<DeviceSample> AppInput::sample(const std::string& id, const Buttons& buttons, DeviceReadStatus* status)
{
    auto reading = request(id, buttons, false);
    if (status)
        *status = reading.status;
    return std::move(reading.sample);
}

CaptureDeviceRead AppInput::capture(const std::string& id)
{
    return request(id, {}, true);
}

CaptureDeviceRead AppInput::request(const std::string& id, const Buttons& buttons, bool capture)
{
    if (id.empty())
        return {};
    std::scoped_lock lock(state_->mutex);
    auto found = state_->devices.find(id);
    if (found == state_->devices.end()) {
        auto device = std::make_shared<State::Device>(id);
        found = state_->devices.emplace(id, device).first;
        if (!device->wake) {
            device->stop();
        } else {
            // DirectInput and its cooperative window are owned by this worker for one device.
            try {
                std::thread([state = state_, device] {
                    try {
                        device->run(*state);
                    } catch (...) {
                        // A failed reader must not stop the other devices.
                    }
                    device->stop();
                }).detach();
            } catch (const std::system_error&) {
                device->stop();
            }
        }
    }
    return found->second->request(buttons, capture);
}

void AppInput::reset(bool closeInput)
{
    std::scoped_lock lock(state_->mutex);
    // Keep the owner until an in-flight call returns, including across a pause or refresh.
    for (const auto& entry : state_->devices)
        entry.second->reset(closeInput);
}

void AppInput::invalidate()
{
    reset(false);
}

void AppInput::close()
{
    reset(true);
}

} // namespace chrome_axis
