#pragma once

#include "chrome_axis/input.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace chrome_axis {

struct DeviceInventoryResult {
    std::vector<DeviceInfo> devices;
    bool available{};
    std::uint64_t revision{};
};

class DeviceInventory {
public:
    using Scan = std::function<DeviceInventoryResult()>;

    explicit DeviceInventory(Scan scan = {});
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
