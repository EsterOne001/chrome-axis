#include "chrome_axis/shifter.hpp"

#include <algorithm>
#include <utility>

namespace chrome_axis {
namespace {

constexpr unsigned int stableReports = 3;
constexpr unsigned int povStep = 4500;
constexpr unsigned int povFullTurn = 36000;

} // namespace

bool sameShifterInput(const AccessoryBinding& first, const AccessoryBinding& second) noexcept
{
    if (first.deviceId != second.deviceId || first.kind != second.kind || first.index != second.index)
        return false;
    if (first.kind != AccessoryKind::Axis)
        return first.rest == second.rest && first.engaged == second.engaged;
    return (first.engaged > first.rest && second.engaged > second.rest)
        || (first.engaged < first.rest && second.engaged < second.rest);
}

ShifterCapture::ShifterCapture(std::string deviceId)
    : deviceId_(std::move(deviceId))
{
}

ShifterCapture::DigitalState ShifterCapture::digitalState(const DeviceSample& sample) noexcept
{
    DigitalState state{sample.buttons, sample.povs};
    for (auto& angle : state.povs) {
        if (angle < povFullTurn)
            angle = static_cast<std::uint16_t>(((angle + povStep / 2) / povStep * povStep) % povFullTurn);
        else
            angle = povCentered;
    }
    return state;
}

bool ShifterCapture::captureNeutral(const DeviceSample& sample) noexcept
{
    if (neutralCaptured_)
        return true;

    const auto state = digitalState(sample);
    if (state != neutral_) {
        neutral_ = state;
        neutralReports_ = 0;
    }
    neutralCaptured_ = ++neutralReports_ == stableReports;
    armed_ = neutralCaptured_;
    return neutralCaptured_;
}

void ShifterCapture::clearSignal() noexcept
{
    pending_.reset();
    signalReports_ = 0;
}

void ShifterCapture::resetPosition() noexcept
{
    neutralReports_ = 0;
    armed_ = false;
    clearSignal();
}

ShifterCaptureResult ShifterCapture::sample(const DeviceSample& sample,
                                            std::span<const AccessoryBinding> existing)
{
    using Status = ShifterCaptureStatus;
    if (!neutralCaptured_)
        return {Status::AwaitingNeutral, {}};

    const auto state = digitalState(sample);
    if (!armed_) {
        if (state != neutral_) {
            neutralReports_ = 0;
            return {Status::AwaitingNeutral, {}};
        }
        armed_ = ++neutralReports_ == stableReports;
        return {armed_ ? Status::AwaitingInput : Status::AwaitingNeutral, {}};
    }

    // Ignore unchanged inputs on a shared wheel base, such as a latched toggle.
    const auto buttons = state.buttons & ~neutral_.buttons;
    std::optional<Signal> candidate;
    bool ambiguous = buttons.count() > 1;
    if (buttons.any() && !ambiguous) {
        for (std::size_t i = 0; i < buttonCount; ++i) {
            if (buttons[i]) {
                candidate = Signal{AccessoryKind::Button, i, 1};
                break;
            }
        }
    }
    for (std::size_t i = 0; i < povCount && !ambiguous; ++i) {
        if (state.povs[i] == povCentered || state.povs[i] == neutral_.povs[i])
            continue;
        // A mapped POV direction needs a centered rest state at runtime.
        ambiguous = candidate.has_value() || neutral_.povs[i] != povCentered;
        if (!ambiguous)
            candidate = Signal{AccessoryKind::Pov, i, state.povs[i]};
    }
    if (ambiguous || !candidate) {
        clearSignal();
        return {ambiguous ? Status::Ambiguous : Status::AwaitingInput, {}};
    }
    if (candidate != pending_) {
        pending_ = candidate;
        signalReports_ = 0;
    }
    if (++signalReports_ < stableReports)
        return {Status::AwaitingInput, {}};

    resetPosition();
    const long rest = candidate->kind == AccessoryKind::Pov ? povCentered : 0;
    AccessoryBinding binding{deviceId_, candidate->kind, candidate->index, rest, candidate->engaged, 0};
    if (std::ranges::any_of(existing, [&](const AccessoryBinding& other) {
        return sameShifterInput(binding, other);
    })) {
        return {Status::Duplicate, {}};
    }
    return {Status::Captured, std::move(binding)};
}

} // namespace chrome_axis
