#include "chrome_axis/simulation.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>

namespace {

bool equal(float actual, float expected)
{
    return std::abs(actual - expected) < 0.0001f;
}

bool neutral(const chrome_axis::Controls& value)
{
    return value.steering == 0.0f && value.throttle == 0.0f && value.brake == 0.0f;
}

int testControls()
{
    chrome_axis::Simulator simulator;
    const chrome_axis::SimulationKeys pressing{.left = true, .throttle = true, .brake = true};
    if (!neutral(simulator.update(pressing, 0.1f)) || !neutral(simulator.update({}, 0.1f)))
        return 1;

    auto value = simulator.update(pressing, 0.05f);
    if (!equal(value.steering, -0.15f) || !equal(value.throttle, 0.15f) || !equal(value.brake, 0.2f))
        return 2;

    value = simulator.update(pressing, 1.0f);
    if (!equal(value.steering, -0.45f) || !equal(value.throttle, 0.45f) || !equal(value.brake, 0.6f))
        return 3;

    for (int i = 0; i < 10; ++i)
        value = simulator.update(pressing, 0.1f);
    if (!equal(value.steering, -1.0f) || !equal(value.throttle, 1.0f) || !equal(value.brake, 1.0f))
        return 4;

    value = simulator.update({.left = true, .right = true}, 0.05f);
    if (!equal(value.steering, -0.75f) || value.throttle != 0.0f || value.brake != 0.0f)
        return 5;

    simulator.reset();
    if (!neutral(simulator.update(pressing, 0.1f)) || !neutral(simulator.update({}, 0.1f)))
        return 6;
    return equal(simulator.update({.right = true}, 0.1f).steering, 0.3f) ? 0 : 7;
}

int testSession()
{
    namespace fs = std::filesystem;
    using chrome_axis::SimulationPublisher;
    using chrome_axis::SimulationReceiver;
    using chrome_axis::SimulationSession;
    const auto temp = fs::temp_directory_path() /
        (L"chrome-axis-simulation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto gameA = temp / L"GameA";
    const auto gameB = temp / L"GameB";
    const struct TestDirectories {
        fs::path path;
        ~TestDirectories()
        {
            std::error_code error;
            fs::remove(path / L"GameA", error);
            fs::remove(path / L"GameB", error);
            fs::remove(path, error);
        }
    } cleanup{temp};
    fs::create_directories(gameA);
    fs::create_directories(gameB);

    SimulationPublisher portable(fs::path{});
    SimulationReceiver empty(fs::path{});
    if (portable.enable() || empty.session() != SimulationSession::inactive)
        return 8;
    SimulationPublisher missing(temp / L"missing");
    if (missing.enable())
        return 14;

    const auto aliasA = gameB / L".." / L"GameA";
    SimulationReceiver beforePublish(gameA);
    if (beforePublish.session() != SimulationSession::inactive)
        return 15;

    SimulationPublisher first(gameA), second(aliasA), other(gameB);
    SimulationReceiver receiver(aliasA), otherReceiver(gameB);
    if (!first.enable() || !first.enabled() || second.enable() || second.enabled() ||
        receiver.session() != SimulationSession::active)
        return 9;
    // The same Windows session can host separate publisher and receiver pairs.
    SimulationReceiver unmatched(gameB);
    if (unmatched.session() != SimulationSession::inactive || !other.enable() ||
        otherReceiver.session() != SimulationSession::active)
        return 16;
    other.disable();
    if (otherReceiver.session() != SimulationSession::inactive || receiver.session() != SimulationSession::active)
        return 17;
    first.disable();
    // A new publisher requires neutral arming without a physical USB handover.
    if (!second.enable() || receiver.session() != SimulationSession::restarted ||
        receiver.session() != SimulationSession::active)
        return 10;

    // The reader must fall back even if the stopped publisher cannot clear its mapping.
    Sleep(400);
    if (receiver.session() != SimulationSession::inactive)
        return 11;
    second.heartbeat();
    if (receiver.session() != SimulationSession::active)
        return 12;
    second.disable();
    return receiver.session() == SimulationSession::inactive ? 0 : 13;
}

} // namespace

int main()
{
    int result = testControls();
    if (result == 0)
        result = testSession();
    if (result != 0)
        std::cerr << "Simulation regression, check " << result << '\n';
    return result;
}
