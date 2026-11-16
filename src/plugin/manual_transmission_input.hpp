#pragma once

#include <optional>

namespace chrome_axis {

struct TransmissionIntent {
    bool manual{};
    int delta{};
    std::optional<int> gear;
};

// Used on the game thread. Hardware input is already debounced and rearmed by AccessoryReader.
class ManualTransmissionInput {
public:
    TransmissionIntent update(bool hardware, std::optional<int> hardwareGear, bool simulation,
                              bool toggle, bool up, bool down, std::optional<int> keyboardGear) noexcept {
        if (hardware != hardware_ || simulation != simulation_) {
            *this = {};
            hardware_ = hardware;
            simulation_ = simulation;
            manualSelected_ = hardware;
        }

        if (!toggle) {
            toggleArmed_ = true;
        } else if (toggleArmed_) {
            toggleArmed_ = false;
            manualSelected_ = !manualSelected_;
        }

        const int delta = sequentialShift(up, down);
        if (hardware)
            return {manualSelected_ && hardwareGear.has_value(), 0, hardwareGear};
        if (!simulation || !manualSelected_) {
            directArmed_ = false;
            keyboardSelection_.reset();
            return {};
        }
        if (!keyboardGear)
            directArmed_ = true;
        if (directArmed_ && keyboardGear)
            keyboardSelection_ = keyboardGear;
        if (keyboardGear)
            return {true, 0, keyboardSelection_};
        if (delta)
            keyboardSelection_.reset();
        return {true, delta, keyboardSelection_};
    }

    void suspend() noexcept {
        toggleArmed_ = false;
        shiftArmed_ = false;
        directArmed_ = false;
        keyboardSelection_.reset();
        up_ = false;
        down_ = false;
    }

private:
    int sequentialShift(bool up, bool down) noexcept {
        if (!shiftArmed_ || (up && down)) {
            shiftArmed_ = !up && !down;
            up_ = down_ = false;
            return 0;
        }
        const int delta = static_cast<int>(up && !up_) - static_cast<int>(down && !down_);
        up_ = up;
        down_ = down;
        return delta;
    }

    bool hardware_{};
    bool simulation_{};
    bool manualSelected_{};
    bool toggleArmed_{};
    bool shiftArmed_{};
    bool directArmed_{};
    std::optional<int> keyboardSelection_;
    bool up_{};
    bool down_{};
};

} // namespace chrome_axis
