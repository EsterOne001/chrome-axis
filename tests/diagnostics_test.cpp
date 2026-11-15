#include "chrome_axis/diagnostics.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        (L"chrome-axis-diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove(path / L"one", error);
        std::filesystem::remove(path / L"two", error);
        std::filesystem::remove(path, error);
    }
};

int testPerformanceCapture(const std::filesystem::path& root)
{
    using namespace chrome_axis;
    DiagnosticReader owner(root);
    DiagnosticReader observer(root);
    DiagnosticSnapshot snapshot;
    snapshot.mode = DiagnosticMode::gameNotForeground;
    if (owner.requestPerformanceCapture(true))
        return 13;
    {
        DiagnosticPublisher publisher(root);
        publisher.publish(snapshot);
        owner.read();
        observer.read();
        const auto start = owner.requestPerformanceCapture(true);
        const auto startRequest = publisher.performanceRequest();
        if (!start || !owner.ownsPerformanceCapture() || !startRequest || startRequest->id != start ||
            !startRequest->enabled || startRequest->leaseUntil <= GetTickCount64() ||
            observer.requestPerformanceCapture(false) || observer.requestPerformanceCapture(true))
            return 14;

        PerformanceReport performance;
        performance.state = PerformanceState::capturing;
        performance.requestId = start;
        performance.startedAt = GetTickCount64();
        performance.usbRead.add(300);
        performance.usbRead.add(500);
        performance.feedback.add(200);
        performance.inputAge.add(1500);
        performance.motionAge.add(4500);
        performance.motionAge.add(18500);
        performance.expiredReads = 2;
        performance.transientReads = 3;
        performance.forceSamples = 20;
        performance.clippedSamples = 3;
        performance.impactClippedSamples = 2;
        performance.submittedEffects = 13;
        performance.peakRequestedForce = 1.625f;
        performance.peakLimitedForce = 1.0f;
        performance.peakSubmittedForce = 0.875f;
        publisher.publish(snapshot, 1, performance);
        const auto live = observer.read();
        if (observer.ownsPerformanceCapture() || live.performance.state != PerformanceState::capturing ||
            live.performance.requestId != start || live.performance.usbRead.samples != 2 ||
            live.performance.usbRead.totalMicros != 800 || live.performance.usbRead.maxMicros != 500 ||
            live.performance.feedback.totalMicros != 200 || live.performance.inputAge.maxMicros != 1500 ||
            live.performance.expiredReads != 2 || live.performance.transientReads != 3 ||
            live.performance.motionAge.samples != 2 || live.performance.motionAge.totalMicros != 23000 ||
            live.performance.motionAge.maxMicros != 18500 || live.performance.forceSamples != 20 ||
            live.performance.clippedSamples != 3 || live.performance.impactClippedSamples != 2 ||
            live.performance.submittedEffects != 13 || live.performance.peakRequestedForce != 1.625f ||
            live.performance.peakLimitedForce != 1.0f || live.performance.peakSubmittedForce != 0.875f)
            return 15;

        const auto stop = owner.requestPerformanceCapture(false);
        const auto stopRequest = publisher.performanceRequest();
        if (stop <= start || owner.ownsPerformanceCapture() || !stopRequest || stopRequest->id != stop ||
            stopRequest->enabled || stopRequest->leaseUntil)
            return 16;
        performance.requestId = stop;
        performance.state = PerformanceState::complete;
        performance.endedAt = GetTickCount64();
        publisher.publish(snapshot, 2, performance);
        const auto finished = owner.read().performance;
        if (finished.state != PerformanceState::complete || finished.requestId != stop ||
            finished.startedAt != performance.startedAt || finished.endedAt != performance.endedAt ||
            finished.usbRead.totalMicros != 800 || finished.motionAge.totalMicros != 23000 ||
            finished.clippedSamples != 3 || finished.impactClippedSamples != 2 || finished.submittedEffects != 13)
            return 17;

        {
            DiagnosticReader closing(root);
            closing.read();
            if (closing.requestPerformanceCapture(true) <= stop)
                return 18;
        }
        const auto closedRequest = publisher.performanceRequest();
        if (!closedRequest || closedRequest->id <= stop || closedRequest->enabled)
            return 19;

        if (observer.requestPerformanceCapture(true) <= closedRequest->id)
            return 20;
    }

    DiagnosticPublisher restarted(root);
    restarted.publish(snapshot);
    const auto restartedRequest = restarted.performanceRequest();
    if (observer.requestPerformanceCapture(false) || observer.read().performance.state != PerformanceState::idle ||
        observer.ownsPerformanceCapture() || !restartedRequest || restartedRequest->id)
        return 21;
    return 0;
}

int run()
{
    using namespace chrome_axis;
    const TemporaryDirectory temp;
    const auto root = temp.path / L"one";
    const auto otherRoot = temp.path / L"two";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(otherRoot);
    DiagnosticReader missing(std::filesystem::path{});
    DiagnosticReader reader(root);
    DiagnosticReader other(otherRoot);
    if (missing.read().connection != DiagnosticConnection::unavailable ||
        reader.read().connection != DiagnosticConnection::disconnected ||
        other.read().connection != DiagnosticConnection::disconnected)
        return 1;

    DiagnosticSnapshot snapshot;
    snapshot.mode = DiagnosticMode::usb;
    snapshot.profile[0] = 'A';
    snapshot.axes[0] = {DiagnosticAxisState::armed, -0.75f, GetTickCount64()};
    snapshot.axes[0].inputAvailable = 1;
    snapshot.axes[0].inputValue = 0.0f;
    snapshot.axes[0].requestedAvailable = 1;
    snapshot.axes[0].requestedValue = 0.0f;
    snapshot.axes[0].delivery = DiagnosticDelivery::gameKept;
    snapshot.axes[1].state = DiagnosticAxisState::waitingForNeutral;
    snapshot.axes[1].finalValue = 0.0f;
    snapshot.axes[1].finalAvailable = 1;
    snapshot.axes[2].state = DiagnosticAxisState::armed;
    snapshot.axes[2].inputValue = 0.25f;
    snapshot.axes[2].inputAvailable = 1;
    snapshot.axes[2].requestedValue = 0.125f;
    snapshot.axes[2].requestedAvailable = 1;
    snapshot.axes[2].finalValue = 0.5f;
    snapshot.axes[2].finalAvailable = 1;
    snapshot.axes[2].delivery = DiagnosticDelivery::gameKept;
    snapshot.pedalSampledAt = snapshot.axes[0].appliedAt;
    snapshot.profileLoaded = 1;
    snapshot.accessoryCount = 2;
    snapshot.accessoryArmed = 1;
    snapshot.accessories = DiagnosticAccessoryState::deviceUnavailable;
    snapshot.lastAccessoryKey = VK_SPACE;
    snapshot.accessorySentAt = snapshot.axes[0].appliedAt;
    snapshot.accessoryCommands[0] = {
        .kind = AccessoryKind::Button,
        .role = ShifterRole::Upshift,
        .activation = AccessoryActivation::pulse,
        .index = 7,
        .key = VK_ADD,
        .engaged = 1,
        .state = DiagnosticCommandState::waitingForNeutral,
        .output = DiagnosticCommandOutput::blocked,
        .active = 1,
        .activeAvailable = 1,
    };
    snapshot.accessoryCommands[1] = {
        .kind = AccessoryKind::Axis,
        .index = 2,
        .key = VK_SPACE,
        .rest = 1000,
        .engaged = 500,
        .state = DiagnosticCommandState::ready,
        .output = DiagnosticCommandOutput::held,
        .active = 1,
        .activeAvailable = 1,
        .travelAvailable = 1,
        .travel = 0.75f,
        .lastSentAt = snapshot.accessorySentAt,
    };
    snapshot.feedback = DiagnosticFeedbackState::active;
    snapshot.feedbackData = {
        .motionAgeMicros = 16500,
        .submittedAt = snapshot.axes[0].appliedAt,
        .rawRequested = 1.25f,
        .requested = 1.0f,
        .submitted = -0.625f,
        .motionAvailable = 1,
        .requestAvailable = 1,
        .clipped = 1,
        .impactClipped = 1,
        .skidding = 1,
    };
    {
        DiagnosticPublisher publisher(root);
        publisher.publish(snapshot, 1);
        const auto received = reader.read();
        const auto& paddle = received.snapshot.accessoryCommands[0];
        const auto& lever = received.snapshot.accessoryCommands[1];
        if (received.connection != DiagnosticConnection::connected || !received.sampledAt ||
            received.snapshot.mode != snapshot.mode || std::string_view(received.snapshot.profile.data()) != "A" ||
            received.snapshot.axes[0].state != snapshot.axes[0].state ||
            received.snapshot.axes[0].value != snapshot.axes[0].value ||
            received.snapshot.axes[0].appliedAt != snapshot.axes[0].appliedAt ||
            received.snapshot.axes[1].state != snapshot.axes[1].state ||
            received.snapshot.profileLoaded != snapshot.profileLoaded ||
            received.snapshot.accessoryCount != snapshot.accessoryCount ||
            received.snapshot.accessoryArmed != snapshot.accessoryArmed ||
            received.snapshot.accessories != snapshot.accessories ||
            received.snapshot.lastAccessoryKey != snapshot.lastAccessoryKey ||
            received.snapshot.accessorySentAt != snapshot.accessorySentAt ||
            paddle.role != ShifterRole::Upshift || paddle.activation != AccessoryActivation::pulse ||
            paddle.index != 7 || paddle.key != VK_ADD || paddle.state != DiagnosticCommandState::waitingForNeutral ||
            paddle.output != DiagnosticCommandOutput::blocked || !paddle.activeAvailable || !paddle.active ||
            paddle.travelAvailable || paddle.lastSentAt ||
            lever.kind != AccessoryKind::Axis || lever.activation != AccessoryActivation::hold ||
            lever.rest != 1000 || lever.engaged != 500 || lever.output != DiagnosticCommandOutput::held ||
            !lever.travelAvailable || lever.travel != 0.75f || lever.lastSentAt != snapshot.accessorySentAt ||
            received.snapshot.feedback != snapshot.feedback ||
            received.snapshot.feedbackData.motionAgeMicros != 16500 ||
            received.snapshot.feedbackData.submittedAt != snapshot.feedbackData.submittedAt ||
            received.snapshot.feedbackData.rawRequested != 1.25f ||
            received.snapshot.feedbackData.requested != 1.0f || received.snapshot.feedbackData.submitted != -0.625f ||
            !received.snapshot.feedbackData.motionAvailable || !received.snapshot.feedbackData.requestAvailable ||
            !received.snapshot.feedbackData.clipped || !received.snapshot.feedbackData.impactClipped ||
            received.snapshot.feedbackData.airborne ||
            !received.snapshot.feedbackData.skidding ||
            !received.lastDriveAt || received.lastDrive.axes[0].inputAvailable != 1 ||
            received.lastDrive.axes[0].inputValue != 0.0f || received.lastDrive.axes[1].inputAvailable ||
            received.lastDrive.axes[0].requestedAvailable != 1 || received.lastDrive.axes[0].requestedValue != 0.0f ||
            received.lastDrive.axes[0].delivery != DiagnosticDelivery::gameKept ||
            received.lastDrive.pedalSampledAt != snapshot.pedalSampledAt ||
            !received.lastDrive.axes[1].finalAvailable || received.lastDrive.axes[1].finalValue != 0.0f ||
            received.lastDrive.axes[1].requestedAvailable || !received.lastDrive.axes[2].finalAvailable ||
            received.lastDrive.axes[2].inputValue != 0.25f || received.lastDrive.axes[2].requestedValue != 0.125f ||
            received.lastDrive.axes[2].finalValue != 0.5f ||
            received.eventCount != 1 || received.events[0].mode != DiagnosticMode::usb ||
            other.read().connection != DiagnosticConnection::disconnected)
            return 2;

        DiagnosticPublisher duplicate(root);
        auto different = snapshot;
        different.mode = DiagnosticMode::simulation;
        duplicate.publish(different);
        if (reader.read().snapshot.mode != DiagnosticMode::usb)
            return 3;

        auto focused = snapshot;
        focused.axes[0].inputValue = 0.5f;
        focused.feedbackData.motionAgeMicros = 24000;
        focused.feedbackData.airborne = 1;
        focused.feedbackData.skidding = 0;
        publisher.publish(focused, 1);
        if (reader.read().eventCount != 1)
            return 8;

        auto idle = snapshot;
        idle.mode = DiagnosticMode::gameNotForeground;
        idle.axes = {};
        idle.accessoryCommands = {};
        idle.feedback = DiagnosticFeedbackState::inactive;
        idle.feedbackData = {};
        publisher.publish(idle);
        DiagnosticReader openedAfterAltTab(root);
        const auto retained = openedAfterAltTab.read();
        if (retained.connection != DiagnosticConnection::connected ||
            retained.snapshot.mode != DiagnosticMode::gameNotForeground ||
            retained.lastDrive.mode != DiagnosticMode::usb ||
            retained.snapshot.accessoryCommands[1].lastSentAt ||
            retained.lastDrive.accessoryCommands[1].lastSentAt != snapshot.accessorySentAt ||
            retained.lastDrive.axes[0].inputValue != 0.5f || retained.eventCount != 2 ||
            retained.snapshot.feedbackData.motionAvailable || retained.snapshot.feedbackData.submittedAt ||
            retained.lastDrive.feedbackData.motionAgeMicros != 24000 || !retained.lastDrive.feedbackData.airborne ||
            retained.events[1].mode != DiagnosticMode::gameNotForeground)
            return 9;

        for (std::size_t i = 0; i < retained.events.size() + 1; ++i) {
            focused.axes[1].state = i % 2 == 0 ? DiagnosticAxisState::deviceUnavailable : DiagnosticAxisState::armed;
            publisher.publish(focused, 1);
        }
        const auto bounded = reader.read();
        if (bounded.eventCount != bounded.events.size() ||
            bounded.events.front().axes[1] != DiagnosticAxisState::armed ||
            bounded.events.back().axes[1] != DiagnosticAxisState::deviceUnavailable)
            return 10;
        publisher.publish(snapshot, 2);
        const auto nextDrive = reader.read();
        if (nextDrive.eventCount != 1 || nextDrive.lastDrive.axes[1].state != DiagnosticAxisState::waitingForNeutral)
            return 11;

        auto pending = snapshot;
        pending.axes[0].state = DiagnosticAxisState::temporarilyUnavailable;
        pending.axes[0].inputAvailable = 0;
        pending.accessories = DiagnosticAccessoryState::temporarilyUnavailable;
        pending.feedback = DiagnosticFeedbackState::waitingForMotion;
        pending.feedbackData.motionAvailable = 0;
        pending.feedbackData.requestAvailable = 0;
        publisher.publish(pending, 2);
        publisher.publish(snapshot, 2);
        const auto resumed = reader.read();
        if (resumed.eventCount != 3 || resumed.events[1].axes[0] != DiagnosticAxisState::temporarilyUnavailable ||
            resumed.events[1].accessories != DiagnosticAccessoryState::temporarilyUnavailable ||
            resumed.events[1].feedback != DiagnosticFeedbackState::waitingForMotion ||
            resumed.snapshot.axes[0].state != DiagnosticAxisState::armed)
            return 12;

        std::this_thread::sleep_for(1700ms);
        const auto stale = reader.read();
        if (stale.connection != DiagnosticConnection::stale || !stale.lastDriveAt || stale.eventCount != 3)
            return 4;
        publisher.publish(snapshot, 2);
        if (reader.read().connection != DiagnosticConnection::connected)
            return 5;
    }
    if (reader.read().connection != DiagnosticConnection::stale)
        return 6;

    DiagnosticPublisher restarted(root);
    snapshot.mode = DiagnosticMode::waitingForDriver;
    restarted.publish(snapshot);
    const auto restart = reader.read();
    if (restart.connection != DiagnosticConnection::connected ||
        restart.snapshot.mode != DiagnosticMode::waitingForDriver || restart.lastDriveAt ||
        restart.eventCount != 1 || restart.events[0].mode != DiagnosticMode::waitingForDriver)
        return 7;
    return testPerformanceCapture(otherRoot);
}

} // namespace

int main()
{
    const auto result = run();
    if (result)
        std::cerr << "Diagnostic channel regression, check " << result << '\n';
    return result;
}
