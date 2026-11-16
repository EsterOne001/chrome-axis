#include "app_input.hpp"
#include "input_window.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
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
        std::optional<Report> report;
        Buttons requested;
        std::uint64_t delivered{};
        bool queued{};
        bool expired{};
        bool interruption{};
        bool observedSample{};
    };

    State() : wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~State() { if (wake) CloseHandle(wake); }

    void run(const Read& injected)
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
            if (queue.empty()) {
                lock.unlock();
                MsgWaitForMultipleObjectsEx(1, &wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    DispatchMessageW(&message);
                lock.lock();
                continue;
            }

            auto id = std::move(queue.front());
            queue.pop_front();
            auto& selected = devices.at(id);
            auto buttons = std::exchange(selected.requested, {});
            selected.queued = false;
            const auto generation = revision;
            lock.unlock();

            CaptureDeviceRead reading;
            if (injected) {
                reading = injected(id, buttons);
            } else if (window.get()) {
                const auto now = Clock::now();
                if (!driver && now >= nextInitialization) {
                    driver.emplace(window.get());
                    if (!driver->ready()) {
                        driver.reset();
                        nextInitialization = now + initializationRetry;
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
            auto& device = devices.at(id);
            const auto number = ++sequence;
            if (reading.sample) {
                if (reading.sample->sampledAt == Clock::time_point{})
                    reading.sample->sampledAt = Clock::now();
                if (Clock::now() - reading.sample->sampledAt > sampleLifetime) {
                    reading.sample.reset();
                    reading.status = DeviceReadStatus::unavailable;
                } else {
                    reading.sample->sequence = number;
                }
            }
            device.interruption |= reading.status == DeviceReadStatus::resumed ||
                (device.observedSample && reading.status == DeviceReadStatus::unavailable);
            device.observedSample |= reading.sample.has_value();
            device.report = Report{std::move(reading), buttons, number};
        }
    }

    std::mutex mutex;
    HANDLE wake{};
    std::deque<std::string> queue;
    std::unordered_map<std::string, Device> devices;
    std::uint64_t revision{}, sequence{};
    bool stopping{}, closeInput{};
};

AppInput::AppInput(Read read) : state_(std::make_shared<State>())
{
    if (!state_->wake) {
        state_->stopping = true;
        return;
    }
    try {
        // A blocked driver does not hold a window or an object owned by the UI.
        // This worker owns its state and cooperative window, and runs in the EXE.
        std::thread([state = state_, read = std::move(read)] {
            try {
                state->run(read);
            } catch (...) {
                std::scoped_lock lock(state->mutex);
                state->stopping = true;
                state->devices.clear();
                state->queue.clear();
            }
        }).detach();
    } catch (const std::system_error&) {
        state_->stopping = true;
    }
}

AppInput::~AppInput()
{
    {
        std::scoped_lock lock(state_->mutex);
        state_->stopping = true;
    }
    if (state_->wake)
        SetEvent(state_->wake);
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
    std::scoped_lock lock(state_->mutex);
    if (id.empty() || state_->stopping)
        return {};
    auto& device = state_->devices[id];
    device.requested |= buttons;
    if (!device.queued) {
        state_->queue.push_back(id);
        device.queued = true;
        SetEvent(state_->wake);
    }

    const auto& report = device.report;
    if (!report || (buttons & ~report->buttons).any())
        return {.sample = {}, .status = DeviceReadStatus::pending, .updated = false};

    auto reading = report->reading;
    const bool expired = reading.sample && Clock::now() - reading.sample->sampledAt > sampleLifetime;
    if (expired) {
        reading.sample.reset();
        reading.status = DeviceReadStatus::unavailable;
        device.interruption = true;
    }
    if (capture) {
        const bool updated = device.delivered != report->number || device.expired != expired;
        device.delivered = report->number;
        device.expired = expired;
        if (!updated)
            return {.sample = {}, .status = DeviceReadStatus::pending, .updated = false};
    }
    if (reading.sample)
        reading.status = std::exchange(device.interruption, false) ? DeviceReadStatus::resumed : DeviceReadStatus::available;
    return reading;
}

void AppInput::reset(bool closeInput)
{
    std::scoped_lock lock(state_->mutex);
    ++state_->revision;
    state_->devices.clear();
    state_->queue.clear();
    state_->closeInput |= closeInput;
    if (state_->wake)
        SetEvent(state_->wake);
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
