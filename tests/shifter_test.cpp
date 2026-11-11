#include "chrome_axis/shifter.hpp"

#include <iostream>

namespace {

using chrome_axis::AccessoryBinding;
using chrome_axis::AccessoryKind;
using chrome_axis::AccessoryProfile;
using chrome_axis::DeviceSample;
using chrome_axis::ShifterCapture;
using chrome_axis::ShifterCaptureStatus;
using chrome_axis::povCentered;

constexpr auto wheelId = "{11111111-1111-1111-1111-111111111111}";
constexpr auto shifterId = "{22222222-2222-2222-2222-222222222222}";

bool neutral(ShifterCapture& capture, const DeviceSample& released)
{
    return capture.sample(released).status == ShifterCaptureStatus::AwaitingNeutral
        && capture.sample(released).status == ShifterCaptureStatus::AwaitingNeutral
        && capture.sample(released).status == ShifterCaptureStatus::AwaitingInput;
}

bool capturesHPattern()
{
    ShifterCapture capture(wheelId);
    DeviceSample released;
    released.buttons.set(8); // An unrelated wheel-base toggle that stays on at rest.
    released.povs[1] = 18000;
    auto noisy = released;
    noisy.buttons.set(63);
    if (capture.sample(released).status != ShifterCaptureStatus::AwaitingNeutral
        || capture.captureNeutral(noisy) || capture.captureNeutral(released)
        || capture.captureNeutral(released) || !capture.captureNeutral(released))
        return false;

    constexpr std::array<std::size_t, 8> positions{33, 19, 88, 4, 74, 12, 41, 127};
    AccessoryProfile bindings;
    for (const auto index : positions) {
        auto inGear = released;
        inGear.buttons.set(index);
        if (!bindings.empty()) {
            if (capture.sample(inGear, bindings).status != ShifterCaptureStatus::AwaitingNeutral
                || !neutral(capture, released))
                return false;
        }
        if (capture.sample(inGear, bindings).status != ShifterCaptureStatus::AwaitingInput
            || capture.sample(inGear, bindings).status != ShifterCaptureStatus::AwaitingInput)
            return false;
        const auto captured = capture.sample(inGear, bindings);
        const AccessoryBinding expected{wheelId, AccessoryKind::Button, index, 0, 1, 0};
        if (captured.status != ShifterCaptureStatus::Captured || captured.binding != expected)
            return false;
        bindings.push_back(*captured.binding);
    }

    auto firstAgain = released;
    firstAgain.buttons.set(positions.front());
    return neutral(capture, released)
        && capture.sample(firstAgain, bindings).status == ShifterCaptureStatus::AwaitingInput
        && capture.sample(firstAgain, bindings).status == ShifterCaptureStatus::AwaitingInput
        && capture.sample(firstAgain, bindings).status == ShifterCaptureStatus::Duplicate
        && capture.sample(firstAgain, bindings).status == ShifterCaptureStatus::AwaitingNeutral;
}

bool capturesPovDirections()
{
    ShifterCapture capture(shifterId);
    const DeviceSample released;
    capture.captureNeutral(released);
    capture.captureNeutral(released);
    capture.captureNeutral(released);

    auto sample = released;
    sample.povs[3] = 8900;
    sample.buttons.set(72);
    if (capture.sample(sample).status != ShifterCaptureStatus::Ambiguous)
        return false;
    sample.buttons.reset();
    if (capture.sample(sample).status != ShifterCaptureStatus::AwaitingInput)
        return false;
    sample.povs[3] = 9300;
    if (capture.sample(sample).status != ShifterCaptureStatus::AwaitingInput)
        return false;
    sample.povs[3] = 9000;
    const auto right = capture.sample(sample);
    const AccessoryBinding expectedRight{shifterId, AccessoryKind::Pov, 3, povCentered, 9000, 0};
    if (right.status != ShifterCaptureStatus::Captured || right.binding != expectedRight
        || !neutral(capture, released))
        return false;

    const AccessoryProfile previous{*right.binding};
    sample.povs[3] = 35800;
    if (capture.sample(sample, previous).status != ShifterCaptureStatus::AwaitingInput)
        return false;
    sample.povs[3] = 300;
    if (capture.sample(sample, previous).status != ShifterCaptureStatus::AwaitingInput)
        return false;
    sample.povs[3] = 0;
    const auto forward = capture.sample(sample, previous);
    const AccessoryBinding expectedForward{shifterId, AccessoryKind::Pov, 3, povCentered, 0, 0};
    return forward.status == ShifterCaptureStatus::Captured && forward.binding == expectedForward;
}

bool ignoresNoiseAndAxes()
{
    ShifterCapture capture(shifterId);
    DeviceSample released;
    released.axesPresent[0] = true;
    capture.captureNeutral(released);
    capture.captureNeutral(released);
    capture.captureNeutral(released);

    auto analogOnly = released;
    analogOnly.axes[0] = 65535;
    for (int i = 0; i < 5; ++i) {
        const auto result = capture.sample(analogOnly);
        if (result.status != ShifterCaptureStatus::AwaitingInput || result.binding)
            return false;
    }
    auto paddle = released;
    paddle.buttons.set(2);
    if (capture.sample(paddle).status != ShifterCaptureStatus::AwaitingInput
        || capture.sample(paddle).status != ShifterCaptureStatus::AwaitingInput)
        return false;
    auto conflict = paddle;
    conflict.buttons.set(3);
    if (capture.sample(conflict).status != ShifterCaptureStatus::Ambiguous
        || capture.sample(paddle).status != ShifterCaptureStatus::AwaitingInput)
        return false;

    capture.resetPosition();
    if (capture.sample(paddle).status != ShifterCaptureStatus::AwaitingNeutral
        || capture.sample(released).status != ShifterCaptureStatus::AwaitingNeutral
        || capture.sample(paddle).status != ShifterCaptureStatus::AwaitingNeutral
        || !neutral(capture, released)
        || capture.sample(paddle).status != ShifterCaptureStatus::AwaitingInput
        || capture.sample(paddle).status != ShifterCaptureStatus::AwaitingInput)
        return false;
    const AccessoryBinding expected{shifterId, AccessoryKind::Button, 2, 0, 1, 0};
    return capture.sample(paddle).binding == expected;
}

bool comparesPhysicalGestures()
{
    using chrome_axis::sameShifterInput;
    const AccessoryBinding forward{wheelId, AccessoryKind::Axis, 0, 32000, 65000, 'A'};
    const AccessoryBinding forwardAgain{wheelId, AccessoryKind::Axis, 0, 31999, 64000, 'B'};
    const AccessoryBinding reverse{wheelId, AccessoryKind::Axis, 0, 32000, 0, 'C'};
    const AccessoryBinding differentDevice{shifterId, AccessoryKind::Axis, 0, 32000, 65000, 'D'};
    const AccessoryBinding button{wheelId, AccessoryKind::Button, 0, 0, 1, 'E'};
    const AccessoryBinding sameButton{wheelId, AccessoryKind::Button, 0, 0, 1, 'F'};
    return sameShifterInput(forward, forwardAgain) && !sameShifterInput(forward, reverse)
        && !sameShifterInput(forward, differentDevice) && !sameShifterInput(forward, button)
        && sameShifterInput(button, sameButton);
}

} // namespace

int main()
{
    if (!capturesHPattern()) {
        std::cerr << "Shifter capture failed for a shared-device H pattern\n";
        return 1;
    }
    if (!capturesPovDirections()) {
        std::cerr << "Shifter capture failed for quantized POV directions\n";
        return 2;
    }
    if (!ignoresNoiseAndAxes()) {
        std::cerr << "Shifter capture accepted an unstable or unsupported signal\n";
        return 3;
    }
    if (!comparesPhysicalGestures()) {
        std::cerr << "Shifter capture did not distinguish physical gestures\n";
        return 4;
    }
    return 0;
}
