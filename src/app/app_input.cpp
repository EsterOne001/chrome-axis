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
constexpr auto guidedReadLimit = std::chrono::milliseconds(220);
constexpr auto initializationRetry = std::chrono::milliseconds(250);
constexpr auto readerIdleTimeout = std::chrono::seconds(5);
constexpr CaptureDeviceRead pending{.sample = {}, .status = DeviceReadStatus::pending, .updated = false};

} // namespace

struct AppInput::State {
    struct Report {
        CaptureDeviceRead reading;
        Buttons buttons;
        std::uint64_t number;
        Clock::time_point completedAt;
        bool guidedCalibration;
    };

    struct Device {
        enum class Exit { stopped, failed };

        explicit Device(std::string deviceId, bool interrupted) : id(std::move(deviceId)), interruption(interrupted) {}
        ~Device() { if (wake) CloseHandle(wake); }

        Exit run(State& state)
        {
            const InputWindow window;
            if (!window.get() && !state.read)
                return Exit::failed;
            std::optional<DirectInput> driver;
            Clock::time_point nextInitialization{};
            std::unique_lock lock(mutex);
            while (!stopping) {
                if (!std::exchange(queued, false)) {
                    const auto remaining = lastRequest + readerIdleTimeout - Clock::now();
                    if (remaining <= Clock::duration::zero()) {
                        stopping = true;
                        break;
                    }
                    lock.unlock();
                    const auto wait = static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
                    if (MsgWaitForMultipleObjectsEx(1, &wake, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                        return Exit::failed;
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                        DispatchMessageW(&message);
                    lock.lock();
                    continue;
                }

                const auto buttons = std::exchange(requested, {});
                const auto generation = revision;
                const bool guidedCalibration = requestedGuidedCalibration;
                lock.unlock();

                const auto startedAt = Clock::now();
                CaptureDeviceRead reading;
                if (state.read) {
                    reading = state.read(id, buttons);
                } else {
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

                const auto completedAt = Clock::now();
                lock.lock();
                if (stopping || generation != revision)
                    continue;
                const auto number = state.sequence.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reading.sample) {
                    if (reading.sample->sampledAt == Clock::time_point{})
                        reading.sample->sampledAt = startedAt;
                    const auto durationLimit = guidedCalibration ? guidedReadLimit : sampleLifetime;
                    const auto validatedAt = guidedCalibration ? completedAt : Clock::now();
                    if (validatedAt - reading.sample->sampledAt > durationLimit) {
                        reading.sample.reset();
                        reading.status = DeviceReadStatus::unavailable;
                    } else {
                        reading.sample->sequence = number;
                    }
                }
                interruption |= reading.status == DeviceReadStatus::resumed ||
                    (observedSample && reading.status == DeviceReadStatus::unavailable);
                observedSample |= reading.sample.has_value();
                report = Report{std::move(reading), buttons, number, completedAt, guidedCalibration};
            }
            return Exit::stopped;
        }

        CaptureDeviceRead request(const Buttons& buttons, Purpose purpose)
        {
            std::scoped_lock lock(mutex);
            if (stopping)
                return pending;
            lastRequest = Clock::now();
            requested |= buttons;
            const bool capture = purpose != Purpose::preview;
            const bool guidedCalibration = purpose == Purpose::guidedCalibration;
            requestedGuidedCalibration = guidedCalibration;
            if (!std::exchange(queued, true))
                SetEvent(wake);

            if (!report || report->guidedCalibration != guidedCalibration || (buttons & ~report->buttons).any())
                return pending;

            auto reading = report->reading;
            const bool expired = reading.sample &&
                lastRequest - (guidedCalibration ? report->completedAt : reading.sample->sampledAt) > sampleLifetime;
            // A slow successful read can become too old to use before the next one arrives.
            // Capture waits for a fresh result while the reader is still making progress.
            const auto nextReadLimit = guidedCalibration ? guidedReadLimit : sampleLifetime;
            if (expired && capture && lastRequest - report->completedAt < nextReadLimit)
                return pending;
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

        void invalidate()
        {
            std::scoped_lock lock(mutex);
            if (stopping)
                return;
            ++revision;
            report.reset();
            requested.reset();
            delivered = 0;
            queued = deliveredExpiry = interruption = observedSample = false;
        }

        void stop()
        {
            std::scoped_lock lock(mutex);
            stopping = true;
            if (wake)
                SetEvent(wake);
        }

        bool readInterrupted()
        {
            std::scoped_lock lock(mutex);
            return observedSample || interruption;
        }

        const std::string id;
        const HANDLE wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        std::mutex mutex;
        std::optional<Report> report;
        Buttons requested;
        Clock::time_point lastRequest{Clock::now()};
        std::uint64_t revision{}, delivered{};
        bool requestedGuidedCalibration{};
        bool queued{}, deliveredExpiry{}, interruption{}, observedSample{}, stopping{};
    };

    struct Reader {
        std::shared_ptr<Device> device;
        Clock::time_point retryAfter{};
        bool interrupted{};
    };

    explicit State(Read read) : read(std::move(read)) {}

    void finished(Device& device, Device::Exit exit)
    {
        std::scoped_lock lock(mutex);
        const auto found = devices.find(device.id);
        if (found == devices.end() || found->second.device.get() != &device)
            return;
        // Publish the slot only after DirectInput and the cooperative window were destroyed.
        if (exit == Device::Exit::failed)
            found->second = {nullptr, Clock::now() + initializationRetry, device.readInterrupted()};
        else
            devices.erase(found);
    }

    const Read read;
    std::atomic_uint64_t sequence{};
    std::mutex mutex;
    std::unordered_map<std::string, Reader> devices;
};

AppInput::AppInput(Read read) : state_(std::make_shared<State>(std::move(read))) {}

AppInput::~AppInput()
{
    std::scoped_lock lock(state_->mutex);
    for (const auto& entry : state_->devices)
        if (entry.second.device)
            entry.second.device->stop();
    state_->devices.clear();
}

std::optional<DeviceSample> AppInput::sample(const std::string& id, const Buttons& buttons, DeviceReadStatus* status)
{
    auto reading = request(id, buttons, Purpose::preview);
    if (status)
        *status = reading.status;
    return std::move(reading.sample);
}

CaptureDeviceRead AppInput::capture(const std::string& id)
{
    return request(id, {}, Purpose::capture);
}

CaptureDeviceRead AppInput::captureGuidedPosition(const std::string& id)
{
    return request(id, {}, Purpose::guidedCalibration);
}

CaptureDeviceRead AppInput::request(const std::string& id, const Buttons& buttons, Purpose purpose)
{
    if (id.empty())
        return {};
    std::scoped_lock lock(state_->mutex);
    auto& reader = state_->devices[id];
    if (reader.device)
        return reader.device->request(buttons, purpose);
    const auto now = Clock::now();
    if (reader.retryAfter > now)
        return {};

    // The slot stays reserved while a call or its cleanup is in progress.
    const auto device = std::make_shared<State::Device>(id, reader.interrupted);
    if (device->wake) {
        try {
            std::thread([state = state_, device] {
                auto exit = State::Device::Exit::failed;
                try {
                    exit = device->run(*state);
                } catch (...) {
                    // A failed reader must not stop the other devices.
                }
                state->finished(*device, exit);
            }).detach();
            reader.device = device;
            const auto initial = device->request(buttons, purpose);
            // Previews that outlived an idle reader must rearm on fresh samples.
            return purpose != Purpose::preview ? initial : CaptureDeviceRead{};
        } catch (const std::system_error&) {
            // Leave a retry time even when the thread itself could not start.
        }
    }
    reader.retryAfter = Clock::now() + initializationRetry;
    return {};
}

void AppInput::invalidate()
{
    std::scoped_lock lock(state_->mutex);
    for (const auto& entry : state_->devices)
        if (entry.second.device)
            entry.second.device->invalidate();
}

void AppInput::close()
{
    std::scoped_lock lock(state_->mutex);
    std::erase_if(state_->devices, [](const auto& entry) { return !entry.second.device; });
    for (const auto& entry : state_->devices)
        entry.second.device->stop();
}

} // namespace chrome_axis
