#include "chrome_axis/shifter.hpp"

#include <algorithm>
#include <utility>

namespace chrome_axis {
namespace {

constexpr unsigned int stableReports = 3;

bool povReadable(const DeviceSample& sample, std::size_t index) noexcept
{
    return sample.povsPresent[index] && !sample.povsPending[index];
}

} // namespace

void PovCapture::observe(const DeviceSample& sample) noexcept
{
    for (std::size_t i = 0; i < povCount; ++i) {
        if (!povReadable(sample, i))
            centered_.reset(i);
        else if (quantizePov(sample.povs[i]) == povCentered)
            centered_.set(i);
    }
}

ShifterCapture::ShifterCapture(std::string deviceId)
    : deviceId_(std::move(deviceId))
{
}

ShifterCapture::DigitalState ShifterCapture::digitalState(const DeviceSample& sample) noexcept
{
    DigitalState state{sample.buttons};
    for (std::size_t i = 0; i < povCount; ++i)
        state.povs[i] = povReadable(sample, i) ? quantizePov(sample.povs[i]) : povCentered;
    return state;
}

bool ShifterCapture::captureNeutral(const DeviceSample& sample, std::span<const AccessoryBinding> existing) noexcept
{
    if (std::ranges::any_of(existing, [&](const auto& position) {
            return position.deviceId == deviceId_ && position.kind != AccessoryKind::Axis
                && discreteAccessoryLevel(position, sample, false) != DiscreteAccessoryLevel::neutral;
        })) {
        neutralCaptured_ = false;
        resetPosition();
        return false;
    }
    if (neutralCaptured_)
        return true;

    povCapture_.observe(sample);
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

void ShifterCapture::deviceInterrupted() noexcept
{
    neutralCaptured_ = false;
    neutral_ = {};
    povCapture_.reset();
    resetPosition();
}

ShifterCaptureResult ShifterCapture::sample(const DeviceSample& sample,
                                            std::span<const AccessoryBinding> existing)
{
    using Status = ShifterCaptureStatus;
    if (!neutralCaptured_)
        return {Status::AwaitingNeutral, {}};

    povCapture_.observe(sample);
    const auto state = digitalState(sample);
    for (std::size_t i = 0; i < povCount; ++i) {
        if (povCapture_.armed(i) && state.povs[i] == povCentered)
            neutral_.povs[i] = povCentered;
    }
    if (!armed_) {
        bool released = state.buttons == neutral_.buttons;
        for (std::size_t i = 0; i < povCount; ++i) {
            // An unreadable POV must not prevent independent buttons from rearming.
            if (povCapture_.armed(i))
                released &= state.povs[i] == neutral_.povs[i];
        }
        if (!released) {
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
        if (!povCapture_.armed(i) || state.povs[i] == povCentered || state.povs[i] == neutral_.povs[i])
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
