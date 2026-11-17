#pragma once

#include "device_capture.hpp"

#include <functional>
#include <memory>

namespace chrome_axis {

// Each request reads the latest published state and schedules a new reading.
// DirectInput creation, sampling, and release stay on the worker for that device.
class AppInput final : public DeviceSampleSource {
public:
    // An injected reader can be called concurrently for different device IDs.
    using Read = std::function<CaptureDeviceRead(const std::string&, const std::bitset<buttonCount>&)>;

    explicit AppInput(Read read = {});
    ~AppInput() override;
    AppInput(const AppInput&) = delete;
    AppInput& operator=(const AppInput&) = delete;

    std::optional<DeviceSample> sample(const std::string& id, const std::bitset<buttonCount>& buttons = {},
                                      DeviceReadStatus* status = nullptr) override;
    CaptureDeviceRead capture(const std::string& id);
    void invalidate();
    void close();

private:
    CaptureDeviceRead request(const std::string& id, const std::bitset<buttonCount>& buttons, bool capture);
    void reset(bool closeInput);

    struct State;
    std::shared_ptr<State> state_;
};

} // namespace chrome_axis
