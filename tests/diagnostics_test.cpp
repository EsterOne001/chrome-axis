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
    snapshot.axes[1].state = DiagnosticAxisState::waitingForNeutral;
    snapshot.profileLoaded = 1;
    snapshot.accessoryCount = 2;
    snapshot.accessoryArmed = 1;
    snapshot.accessories = DiagnosticAccessoryState::deviceUnavailable;
    snapshot.lastAccessoryKey = VK_SPACE;
    snapshot.accessorySentAt = snapshot.axes[0].appliedAt;
    snapshot.feedback = DiagnosticFeedbackState::active;
    {
        DiagnosticPublisher publisher(root);
        publisher.publish(snapshot);
        const auto received = reader.read();
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
            received.snapshot.feedback != snapshot.feedback ||
            other.read().connection != DiagnosticConnection::disconnected)
            return 2;

        DiagnosticPublisher duplicate(root);
        auto different = snapshot;
        different.mode = DiagnosticMode::simulation;
        duplicate.publish(different);
        if (reader.read().snapshot.mode != DiagnosticMode::usb)
            return 3;

        std::this_thread::sleep_for(1700ms);
        if (reader.read().connection != DiagnosticConnection::stale)
            return 4;
        publisher.publish(snapshot);
        if (reader.read().connection != DiagnosticConnection::connected)
            return 5;
    }
    if (reader.read().connection != DiagnosticConnection::stale)
        return 6;

    DiagnosticPublisher restarted(root);
    snapshot.mode = DiagnosticMode::waitingForDriver;
    restarted.publish(snapshot);
    if (reader.read().connection != DiagnosticConnection::connected ||
        reader.read().snapshot.mode != DiagnosticMode::waitingForDriver)
        return 7;
    return 0;
}

} // namespace

int main()
{
    const auto result = run();
    if (result)
        std::cerr << "Diagnostic channel regression, check " << result << '\n';
    return result;
}
