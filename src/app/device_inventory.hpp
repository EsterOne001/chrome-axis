#pragma once

#include "chrome_axis/input.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace chrome_axis {

struct DeviceInventoryResult {
    std::vector<DeviceInfo> devices;
    bool available{};
    std::uint64_t revision{};
    std::optional<ForceFeedbackProbe> feedbackProbe{};
};

std::optional<std::size_t> inventoryDeviceSelection(std::span<const DeviceInfo> devices, std::string_view preferred,
                                                    bool selectFirstIfUnassigned);

class DeviceInventory {
public:
    using Scan = std::function<DeviceInventoryResult()>;

    explicit DeviceInventory(Scan scan = {});
    explicit DeviceInventory(std::string forceFeedbackDevice, std::size_t steeringAxis);
    ~DeviceInventory();
    DeviceInventory(const DeviceInventory&) = delete;
    DeviceInventory& operator=(const DeviceInventory&) = delete;

    void refresh();
    void refreshAfterDeviceChange();
    void setAutomaticRetries(bool active);
    std::shared_ptr<const DeviceInventoryResult> current() const;

private:
    struct State;
    void schedule(std::chrono::milliseconds delay);
    std::shared_ptr<State> state_;
};

} // namespace chrome_axis
