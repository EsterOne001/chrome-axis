#pragma once

#include "chrome_axis/accessories.hpp"

#include <optional>

namespace chrome_axis {

class ShifterCaptureKey {
public:
    void enter(ShifterRole role, unsigned int suggested) noexcept {
        if (role_ != role) {
            role_ = role;
            key_ = suggested;
        }
    }
    bool hasPosition() const noexcept { return role_.has_value(); }
    unsigned int value() const noexcept { return key_; }
    void choose(unsigned int key) noexcept { key_ = key; }

private:
    std::optional<ShifterRole> role_;
    unsigned int key_{};
};

} // namespace chrome_axis
