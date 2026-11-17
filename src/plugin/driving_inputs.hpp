#pragma once

#include "chrome_axis/accessories.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>

namespace chrome_axis {

struct DrivingInputDevice {
    std::string id;
    DeviceObjectSelection objects;
    std::shared_ptr<AccessoryProfile> discrete;
};

class PaddleInputBatch {
public:
    void observe(const AccessoryTransmissionSample& sample, DeviceReadClock::duration lifetime,
                 std::optional<DeviceReadClock::time_point> splitEvidence = std::nullopt) noexcept {
        if (!sample.sequentialArmed || sample.paddlesPressed > 1) {
            pending_.reset();
            return;
        }
        if (sample.paddlesPressed == 0 || sample.shiftDelta)
            finish();
        // Replaying a historical level preserves holds, but a shift requires
        // evidence from both paddles when the press occurred.
        const auto opposite = splitEvidence.value_or(sample.oppositePaddleSampledAt);
        if (sample.shiftDelta && opposite != DeviceReadClock::time_point{} &&
            opposite <= sample.shiftSampledAt && sample.shiftSampledAt - opposite <= lifetime)
            pending_ = Edge{sample.shiftDelta, sample.shiftSampledAt};
    }

    // A shift becomes deliverable on release, or at the end of the batch
    // if the other paddle has not joined the press.
    void finish() noexcept {
        if (!pending_)
            return;
        if (count_ == edges_.size())
            overflowed_ = true;
        else
            edges_[count_++] = *pending_;
        pending_.reset();
    }

    auto edges() const noexcept { return std::span(edges_).first(count_); }
    bool overflowed() const noexcept { return overflowed_; }

private:
    using Edge = std::pair<int, DeviceReadClock::time_point>;
    std::array<Edge, 4> edges_{};
    std::optional<Edge> pending_;
    std::size_t count_{};
    bool overflowed_{};
};

// Each DirectInput instance and its COM objects stay on their device's thread.
// The consumer copies reports and discrete transitions without calling the driver.
template <class Driver = DirectInput>
class DrivingInputs final : public DeviceSampleSource {
public:
    using Clock = DeviceReadClock;

    struct Event {
        DirectInputReadEventKind kind{};
        std::array<char, 40> operation{};
        HRESULT result{};
        std::uint64_t durationMicros{};
        std::uint64_t sampledAt{};
    };

    struct Cycle {
        Clock::time_point finishedAt{};
        std::uint64_t durationMicros{};
        bool missedCadence{};
    };

    DrivingInputs(Clock::duration interval, Clock::duration lifetime, DirectInputDiagnostic logger = nullptr)
        : interval_(interval), lifetime_(lifetime), logger_(logger) {}

    ~DrivingInputs() override { requestStop(); }

    void requestStop() noexcept {
        for (auto& [id, entry] : active_)
            entry.worker->stop();
        for (auto& worker : retired_)
            worker->stop();
    }

    // Returns false if a previous worker is still closing this device.
    bool configure(HWND window, std::span<const DrivingInputDevice> devices) {
        reap();
        std::array<const DrivingInputDevice*, 2> paddles{};
        for (const auto& device : devices) {
            if (!device.discrete)
                continue;
            for (const auto& binding : *device.discrete)
                if (binding.nativeSequential)
                    paddles[binding.role == ShifterRole::Upshift ? 0 : 1] = &device;
        }
        const bool split = paddles[0] && paddles[1] && paddles[0] != paddles[1];
        splitPaddles_ = {};

        for (auto entry = active_.begin(); entry != active_.end();) {
            if (std::ranges::find(devices, entry->first, &DrivingInputDevice::id) != devices.end()) {
                ++entry;
                continue;
            }
            entry->second.worker->stop();
            retired_.push_back(std::move(entry->second.worker));
            entry = active_.erase(entry);
        }
        bool configured = true;
        for (const auto& device : devices) {
            auto found = active_.find(device.id);
            if (found == active_.end()) {
                if (std::ranges::any_of(retired_, [&](const auto& worker) { return worker->id() == device.id; })) {
                    configured = false;
                    continue;
                }
                auto worker = std::make_unique<Worker>(device.id, interval_, lifetime_, logger_);
                found = active_.try_emplace(device.id, Entry{std::move(worker)}).first;
            }
            auto& entry = found->second;
            const bool trackPaddle = split && (&device == paddles[0] || &device == paddles[1]);
            const Configuration config{window, device.objects, device.discrete, true, trackPaddle};
            if (trackPaddle)
                splitPaddles_[&device == paddles[0] ? 0 : 1] = &entry;
            if (entry.config != config)
                entry.previousAccessory = {};
            entry.config = config;
            entry.worker->configure(entry.config);
        }
        replays_.reserve(devices.size() * discreteQueueCapacity);
        return configured;
    }

    void pause() {
        for (auto& [id, entry] : active_) {
            entry.config.active = false;
            entry.worker->pause();
            entry.previousAccessory = {};
        }
        reap();
    }

    void discardAccessorySamples() {
        replays_.clear();
        for (auto& [id, entry] : active_) {
            entry.previousAccessory = {};
            entry.worker->discardAccessorySamples();
        }
    }

    bool ready() const noexcept { return !active_.empty(); }

    void beginPoll(Clock::time_point now) {
        replays_.clear();
        for (auto& [id, entry] : active_) {
            auto& report = entry.report;
            if (entry.worker->finished() && entry.config.active && now >= entry.restartAfter) {
                entry.restartAfter = now + std::chrono::seconds(1);
                try {
                    auto replacement = std::make_unique<Worker>(id, interval_, lifetime_, logger_);
                    replacement->configure(entry.config);
                    entry.worker = std::move(replacement);
                    entry.needsRearm = true;
                } catch (const std::system_error&) {
                    // The other devices remain available if this thread cannot be created.
                }
            }
            const auto start = replays_.size();
            const bool overflow = entry.worker->collect(report, now, [&](const auto& sample) {
                replays_.push_back({&entry, sample});
            });
            entry.resetAccessories = overflow;
            entry.disruptedAccessories = overflow;
            if (overflow)
                replays_.resize(start);
            if (!report.sample || now - report.sample->sampledAt > lifetime_) {
                if (report.sample || (report.status == DeviceReadStatus::pending &&
                    (report.lastValid == Clock::time_point{} || now - report.lastValid > lifetime_)))
                    report.status = DeviceReadStatus::unavailable;
                report.sample.reset();
                entry.needsRearm |= report.status == DeviceReadStatus::unavailable;
                replays_.resize(start);
                entry.resetAccessories |= report.status == DeviceReadStatus::unavailable;
                entry.disruptedAccessories = true;
                continue;
            }
            if (entry.needsRearm || entry.seenLoss != report.loss) {
                report.status = DeviceReadStatus::resumed;
                entry.needsRearm = false;
                entry.seenLoss = report.loss;
                replays_.resize(start);
                entry.resetAccessories = true;
                entry.disruptedAccessories = true;
            } else {
                report.status = DeviceReadStatus::available;
            }
        }
        std::ranges::sort(replays_, {}, [](const auto& frame) {
            return std::pair{frame.sample.sampledAt, frame.sample.sequence};
        });
    }

    template <class Visit>
    void visitAccessoryDisruptions(Visit&& visit) const {
        for (const auto& [id, entry] : active_)
            if (entry.config.discrete && entry.disruptedAccessories)
                visit(id, entry.resetAccessories);
    }

    // Replay discrete changes against the prior per-device state. The final
    // visit publishes current levels, including the continuous accessory axes.
    template <class Visit>
    void replayAccessories(Visit&& visit) {
        if (!replays_.empty()) {
            for (auto& [id, entry] : active_) {
                entry.replay = entry.report;
                if (entry.config.discrete && !entry.disruptedAccessories && entry.previousAccessory.sample) {
                    // The current report confirms continuity. The previous
                    // report provides the starting level for the replay, even
                    // when it has aged out as a source of new commands.
                    entry.replay = entry.previousAccessory;
                    entry.replay.status = DeviceReadStatus::available;
                }
            }
            replaying_ = true;
            for (auto frame = replays_.begin(); frame != replays_.end();) {
                const auto timestamp = frame->sample.sampledAt;
                do {
                    frame->entry->replay.sample = frame->sample;
                    frame->entry->replay.status = DeviceReadStatus::available;
                    ++frame;
                } while (frame != replays_.end() && frame->sample.sampledAt == timestamp);
                visit(false);
            }
            replaying_ = false;
        }
        replays_.clear();
        visit(true);
        for (auto& [id, entry] : active_)
            if (entry.config.discrete)
                entry.previousAccessory = entry.report;
    }

    void observePaddles(PaddleInputBatch& batch, const AccessoryTransmissionSample& sample,
                        Clock::duration lifetime) {
        // The replay reconstructs levels, not the sampling cadence of a paddle
        // on another device. Use that worker's report at the physical edge.
        if (sample.shiftDelta && (splitPaddles_[0] || splitPaddles_[1])) {
            const auto* opposite = splitPaddles_[sample.shiftDelta > 0 ? 1 : 0];
            const auto proof = opposite && !opposite->disruptedAccessories
                ? opposite->worker->paddleEvidenceAt(sample.shiftSampledAt, opposite->report.loss)
                : Clock::time_point{};
            batch.observe(sample, lifetime, proof);
        } else {
            batch.observe(sample, lifetime);
        }
    }

    std::optional<DeviceSample> sample(const std::string& id, const std::bitset<buttonCount>&,
                                       DeviceReadStatus* status) override {
        const auto it = active_.find(id);
        if (it == active_.end()) {
            if (status)
                *status = DeviceReadStatus::unavailable;
            return std::nullopt;
        }
        const auto& entry = it->second;
        const auto& report = replaying_ ? entry.replay : entry.report;
        if (status)
            *status = report.status;
        return report.sample;
    }

    std::uint64_t recoveryToken(const std::string& id) const noexcept {
        const auto it = active_.find(id);
        return it != active_.end() && it->second.report.sample
            ? it->second.report.sample->recoveryToken : 0;
    }

    template <class Visit>
    void drainEvents(Visit&& visit) {
        for (auto& [id, entry] : active_)
            while (const auto event = entry.worker->nextEvent())
                visit(id, *event);
        for (auto& worker : retired_)
            while (const auto event = worker->nextEvent())
                visit(worker->id(), *event);
    }

    template <class Visit>
    void drainCycles(Visit&& visit) {
        for (auto& [id, entry] : active_)
            while (const auto cycle = entry.worker->nextCycle())
                visit(*cycle);
    }

    template <class Visit>
    void visitActiveCalls(Visit&& visit) const {
        constexpr std::uint64_t slowCallMilliseconds = 250;
        const auto now = GetTickCount64();
        const auto report = [&](const auto& worker) {
            for (const bool layoutProbe : {false, true}) {
                const auto call = worker.activeCall(layoutProbe);
                if (call.overdue(now, slowCallMilliseconds))
                    visit(std::string_view(worker.id()), std::string_view(call.operation), call.startedAt, layoutProbe);
            }
        };
        for (const auto& [id, entry] : active_)
            report(*entry.worker);
        for (const auto& worker : retired_)
            report(*worker);
    }

private:
    static constexpr std::size_t discreteQueueCapacity = 16;
    // Covers the 100 ms press lifetime plus its preceding 100 ms of paddle
    // evidence at the production 5 ms cadence, without spending FIFO slots.
    static constexpr std::size_t paddleEvidenceCapacity = 64;

    struct Configuration {
        HWND window{};
        DeviceObjectSelection objects;
        std::shared_ptr<const AccessoryProfile> discrete;
        bool active{};
        bool trackPaddle{};

        bool operator==(const Configuration&) const = default;
    };

    struct Report {
        std::optional<DeviceSample> sample;
        DeviceReadStatus status{DeviceReadStatus::unavailable};
        std::uint64_t loss{};
        Clock::time_point lastValid{};
    };

    struct PaddleEvidence {
        Clock::time_point sampledAt{}, completedAt{};
        DiscreteAccessoryLevel level{};
    };

    template <class Record, std::size_t capacity>
    class Recent {
    public:
        bool full() const noexcept { return size_ == capacity; }
        void clear() noexcept { size_ = 0; }

        void push(const Record& record) noexcept {
            if (size_ == capacity)
                pop();
            slots_[(first_ + size_++) % capacity] = record;
        }

        std::optional<Record> pop() noexcept {
            if (!size_)
                return std::nullopt;
            auto result = slots_[first_];
            first_ = (first_ + 1) % capacity;
            --size_;
            return result;
        }

        template <class Predicate>
        const Record* findNewest(Predicate&& matches) const noexcept {
            for (auto count = size_; count > 0; --count) {
                const auto& record = slots_[(first_ + count - 1) % capacity];
                if (matches(record))
                    return &record;
            }
            return nullptr;
        }

    private:
        std::array<Record, capacity> slots_{};
        std::size_t first_{}, size_{};
    };

    class Worker {
    public:
        Worker(std::string id, Clock::duration interval, Clock::duration lifetime, DirectInputDiagnostic logger)
            : id_(std::move(id)), interval_(interval), lifetime_(lifetime), logger_(logger),
              thread_([this](std::stop_token stop) {
                  try {
                      run(stop);
                  } catch (...) {
                      std::scoped_lock lock(mutex_);
                      latest_ = {};
                  }
                  finished_.store(true, std::memory_order_release);
              }) {}

        const std::string& id() const noexcept { return id_; }
        DirectInputActiveCall::Snapshot activeCall(bool layoutProbe) const noexcept {
            return (layoutProbe ? layoutCall_ : activeCall_).snapshot();
        }

        void configure(Configuration config) {
            std::scoped_lock lock(mutex_);
            if (config_ != config) {
                config_ = std::move(config);
                ++revision_;
                latest_ = {};
                discardDiscrete();
                cycles_ = {};
                changed_.notify_one();
            }
        }

        void pause() {
            std::scoped_lock lock(mutex_);
            if (config_.active) {
                config_.active = false;
                ++revision_;
                latest_ = {};
                discardDiscrete();
                cycles_ = {};
                changed_.notify_one();
            }
        }

        void stop() noexcept { thread_.request_stop(); }
        bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }

        void discardAccessorySamples() {
            std::scoped_lock lock(mutex_);
            discardDiscrete();
        }

        template <class Visit>
        bool collect(Report& report, Clock::time_point now, Visit&& visit) {
            std::scoped_lock lock(mutex_);
            report = latest_;
            if (std::exchange(discreteOverflow_, false))
                return true;
            while (const auto sample = discrete_.pop()) {
                if (now - sample->sampledAt > lifetime_) {
                    discrete_.clear();
                    paddleEvidence_.clear();
                    return true;
                }
                visit(*sample);
            }
            return false;
        }

        Clock::time_point paddleEvidenceAt(Clock::time_point pressedAt, std::uint64_t loss) {
            std::scoped_lock lock(mutex_);
            if (discreteOverflow_ || latest_.loss != loss || latest_.status == DeviceReadStatus::unavailable)
                return {};
            const auto* report = paddleEvidence_.findNewest([&](const auto& evidence) {
                return evidence.completedAt <= pressedAt;
            });
            // Examine the latest report before the press, including negative
            // evidence; an older neutral must not hide a later conflict.
            return report && (report->level == DiscreteAccessoryLevel::neutral ||
                              report->level == DiscreteAccessoryLevel::idle) ? report->sampledAt : Clock::time_point{};
        }

        std::optional<Event> nextEvent() {
            std::scoped_lock lock(mutex_);
            return events_.pop();
        }

        std::optional<Cycle> nextCycle() {
            std::scoped_lock lock(mutex_);
            return cycles_.pop();
        }

    private:
        void discardDiscrete() noexcept {
            ++discreteRevision_;
            discrete_.clear();
            paddleEvidence_.clear();
            discreteOverflow_ = false;
            haveDiscreteBaseline_ = false;
        }

        void publishDiscrete(const Report& report, const AccessoryProfile& bindings,
                             bool trackPaddle, Clock::time_point completedAt) {
            if (report.status == DeviceReadStatus::pending)
                return;
            if (!report.sample || report.status == DeviceReadStatus::resumed) {
                discrete_.clear();
                paddleEvidence_.clear();
                haveDiscreteBaseline_ = false;
            }
            if (!report.sample)
                return;
            bool changed = false;
            for (std::size_t i = 0; i < bindings.size(); ++i) {
                auto& previous = discreteLevels_[i];
                const auto current = discreteAccessoryLevel(bindings[i], *report.sample,
                    haveDiscreteBaseline_ && previous == DiscreteAccessoryLevel::engaged);
                changed |= haveDiscreteBaseline_ && current != previous;
                previous = current;
                if (trackPaddle && bindings[i].nativeSequential)
                    paddleEvidence_.push({report.sample->sampledAt, completedAt, current});
            }
            haveDiscreteBaseline_ = true;
            if (!changed || discreteOverflow_)
                return;
            if (discrete_.full()) {
                discrete_.clear();
                paddleEvidence_.clear();
                discreteOverflow_ = true;
                Event event{.kind = DirectInputReadEventKind::discreteOverflow, .sampledAt = GetTickCount64()};
                std::ranges::copy(std::string_view("USB discrete inputs"), event.operation.begin());
                events_.push(event);
                return;
            }
            discrete_.push(*report.sample);
        }

        static void observe(void* opaque, const DirectInputReadEvent& read) noexcept {
            auto& worker = *static_cast<Worker*>(opaque);
            Event event{.kind = read.kind, .result = read.result, .durationMicros = read.durationMicros,
                        .sampledAt = GetTickCount64()};
            std::ranges::copy(read.operation.substr(0, event.operation.size() - 1), event.operation.begin());
            std::scoped_lock lock(worker.mutex_);
            if (read.kind == DirectInputReadEventKind::unavailable && read.operation == "DirectInput8Create") {
                if (worker.lastFactoryFailure_ == read.result)
                    return;
                worker.lastFactoryFailure_ = read.result;
            }
            worker.events_.push(event);
        }

        void run(std::stop_token stop) {
            std::optional<Driver> driver;
            HWND window{};
            Clock::time_point retry{};
            Clock::time_point lastValid{}, previousStart{};
            std::uint64_t sequence{}, loss{};
            std::uint64_t previousRevision{};
            bool hadReport{};
            while (!stop.stop_requested()) {
                Configuration config;
                std::uint64_t revision, discreteRevision;
                Clock::time_point started;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, stop, [&] { return config_.active; });
                    if (stop.stop_requested())
                        break;
                    config = config_;
                    revision = revision_;
                    discreteRevision = discreteRevision_;
                    started = Clock::now();
                }
                const bool missedCadence = previousRevision == revision && previousStart != Clock::time_point{} &&
                    started - previousStart > interval_ + std::chrono::milliseconds(1);
                previousStart = started;
                previousRevision = revision;
                if (window != config.window) {
                    driver.reset();
                    window = config.window;
                    retry = {};
                    hadReport = false;
                    lastValid = {};
                }
                if (!driver && started >= retry) {
                    driver.emplace(window, logger_, &observe, this, &activeCall_, &layoutCall_);
                    if (!driver->ready()) {
                        driver.reset();
                        retry = Clock::now() + std::chrono::seconds(1);
                    }
                }
                Report report;
                if (driver) {
                    if constexpr (requires { driver->sampleForDriving(id_, config.objects, report.status); })
                        report.sample = driver->sampleForDriving(id_, config.objects, report.status);
                    else
                        report.sample = driver->sampleConfigured(id_, config.objects, report.status);
                }
                const auto finishedAt = Clock::now();
                if (report.sample && finishedAt - report.sample->sampledAt > lifetime_) {
                    observe(this, {DirectInputReadEventKind::sampleExpired, id_, "USB sample", S_OK,
                        elapsedMicros(finishedAt - report.sample->sampledAt)});
                    report.sample.reset();
                    report.status = DeviceReadStatus::unavailable;
                }
                if ((!report.sample && report.status == DeviceReadStatus::unavailable && hadReport) ||
                    report.status == DeviceReadStatus::resumed)
                    ++loss;
                if (report.sample) {
                    report.sample->sequence = ++sequence;
                    lastValid = report.sample->sampledAt;
                }
                if (report.status != DeviceReadStatus::pending)
                    hadReport = report.sample.has_value();
                report.loss = loss;
                report.lastValid = lastValid;
                const auto interval = report.status == DeviceReadStatus::unavailable
                    ? std::max(interval_, std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(25)))
                    : interval_;
                {
                    std::unique_lock lock(mutex_);
                    if (driver)
                        lastFactoryFailure_.reset();
                    if (revision == revision_) {
                        if (config.discrete && discreteRevision == discreteRevision_)
                            publishDiscrete(report, *config.discrete, config.trackPaddle, finishedAt);
                        latest_ = std::move(report);
                        cycles_.push({finishedAt, elapsedMicros(finishedAt - started), missedCadence});
                    }
                    changed_.wait_until(lock, stop, started + interval, [&] { return revision != revision_; });
                }
            }
        }

        static std::uint64_t elapsedMicros(Clock::duration elapsed) noexcept {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        }

        const std::string id_;
        const Clock::duration interval_, lifetime_;
        DirectInputDiagnostic logger_{};
        std::mutex mutex_;
        std::condition_variable_any changed_;
        Configuration config_{};
        Report latest_;
        Recent<DeviceSample, discreteQueueCapacity> discrete_;
        Recent<PaddleEvidence, paddleEvidenceCapacity> paddleEvidence_;
        std::array<DiscreteAccessoryLevel, maxAccessoryBindings> discreteLevels_{};
        bool haveDiscreteBaseline_{};
        Recent<Event, 8> events_;
        Recent<Cycle, 32> cycles_;
        std::optional<HRESULT> lastFactoryFailure_;
        DirectInputActiveCall activeCall_, layoutCall_;
        std::uint64_t revision_{};
        std::uint64_t discreteRevision_{};
        bool discreteOverflow_{};
        std::atomic<bool> finished_{};
        std::jthread thread_;
    };

    struct Entry {
        std::unique_ptr<Worker> worker;
        Configuration config{};
        Report report{};
        Report previousAccessory{};
        Report replay{};
        Clock::time_point restartAfter{};
        std::uint64_t seenLoss{};
        bool needsRearm{};
        bool resetAccessories{};
        bool disruptedAccessories{};
    };

    struct Replay {
        Entry* entry{};
        DeviceSample sample;
    };

    void reap() {
        std::erase_if(retired_, [](const auto& worker) { return worker->finished(); });
    }

    Clock::duration interval_, lifetime_;
    DirectInputDiagnostic logger_{};
    std::unordered_map<std::string, Entry> active_;
    std::array<Entry*, 2> splitPaddles_{}; // Node addresses survive rehash; configure rebuilds the pair.
    std::vector<std::unique_ptr<Worker>> retired_;
    std::vector<Replay> replays_;
    bool replaying_{};
};

} // namespace chrome_axis
