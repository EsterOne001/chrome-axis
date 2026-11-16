#define DIRECTINPUT_VERSION 0x0800

#include <chrome_axis/input.hpp>

#include "device_object_probe.hpp"
#include "device_layout_worker.hpp"
#include "device_read_recovery.hpp"
#include "wheel_force_retry.hpp"

#include <dinput.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace chrome_axis {
namespace {

using Clock = std::chrono::steady_clock;

// A temporarily missing USB report can preserve arming, never the old input itself.
constexpr auto pendingGrace = std::chrono::milliseconds(100);
constexpr auto slowInputOperation = std::chrono::milliseconds(20);
constexpr auto slowInputReportInterval = std::chrono::seconds(1);
constexpr auto forceDuration = std::chrono::milliseconds(100);
constexpr auto forceRefresh = std::chrono::milliseconds(60);
std::atomic<std::uint64_t> hardwareRecoverySerial{};
static_assert(forceRefresh < forceDuration);
constexpr std::array<const char*, axisCount> axisNames{
    "X", "Y", "Z", "Rx", "Ry", "Rz", "Slider0", "Slider1"
};
constexpr std::array<DWORD, axisCount> axisOffsets{
    offsetof(DIJOYSTATE2, lX), offsetof(DIJOYSTATE2, lY),
    offsetof(DIJOYSTATE2, lZ), offsetof(DIJOYSTATE2, lRx),
    offsetof(DIJOYSTATE2, lRy), offsetof(DIJOYSTATE2, lRz),
    offsetof(DIJOYSTATE2, rglSlider), offsetof(DIJOYSTATE2, rglSlider) + sizeof(LONG)
};

static_assert(buttonCount == std::size(DIJOYSTATE2{}.rgbButtons));
static_assert(povCount == std::size(DIJOYSTATE2{}.rgdwPOV));

struct ReleaseInterface {
    template <class Interface>
    void operator()(Interface* interfacePtr) const noexcept {
        interfacePtr->Release();
    }
};

using InputPtr = std::unique_ptr<IDirectInput8W, ReleaseInterface>;
using DevicePtr = std::unique_ptr<IDirectInputDevice8W, ReleaseInterface>;
using EffectPtr = std::unique_ptr<IDirectInputEffect, ReleaseInterface>;

std::string utf8(const wchar_t* value) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), bytes, nullptr, nullptr) == 0) {
        return {};
    }
    result.pop_back();
    return result;
}

std::string deviceId(const GUID& guid) {
    wchar_t buffer[39]{};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return utf8(buffer);
}

HRESULT parseDeviceId(std::string_view id, GUID& guid) {
    if (!isDeviceId(id)) {
        return E_INVALIDARG;
    }
    const std::wstring wideId(id.begin(), id.end());
    return CLSIDFromString(wideId.c_str(), &guid);
}

DevicePtr createDevice(IDirectInput8W& input, const GUID& id, HRESULT* status = nullptr) {
    IDirectInputDevice8W* device = nullptr;
    const auto result = input.CreateDevice(id, &device, nullptr);
    if (status)
        *status = result;
    return SUCCEEDED(result) ? DevicePtr(device) : DevicePtr{};
}

bool hasObject(IDirectInputDevice8W& device, DWORD offset, DWORD type, HRESULT* status = nullptr) {
    DIDEVICEOBJECTINSTANCEW object{};
    object.dwSize = sizeof(object);
    const auto result = device.GetObjectInfo(&object, offset, DIPH_BYOFFSET);
    if (status)
        *status = result;
    return SUCCEEDED(result) && (object.dwType & type) != 0;
}

std::array<bool, axisCount> availableAxes(IDirectInputDevice8W& device) {
    std::array<bool, axisCount> axes{};
    for (std::size_t index = 0; index < axes.size(); ++index) {
        axes[index] = hasObject(device, axisOffsets[index], DIDFT_ABSAXIS);
    }
    return axes;
}

std::bitset<buttonCount> availableButtons(IDirectInputDevice8W& device) {
    std::bitset<buttonCount> buttons;
    for (DWORD index = 0; index < buttonCount; ++index) {
        buttons[index] = hasObject(device, DIJOFS_BUTTON(index), DIDFT_BUTTON);
    }
    return buttons;
}

std::array<bool, povCount> availablePovs(IDirectInputDevice8W& device) {
    std::array<bool, povCount> povs{};
    for (DWORD index = 0; index < povs.size(); ++index) {
        povs[index] = hasObject(device, DIJOFS_POV(index), DIDFT_POV);
    }
    return povs;
}

bool forceActuator(IDirectInputDevice8W& device, std::size_t axis) {
    DIDEVICEOBJECTINSTANCEW object{};
    object.dwSize = sizeof(object);
    return axis < axisCount && SUCCEEDED(device.GetObjectInfo(&object, axisOffsets[axis], DIPH_BYOFFSET))
        && (object.dwFlags & DIDOI_FFACTUATOR) != 0 && (object.dwType & DIDFT_ABSAXIS) != 0;
}

bool supportsConstantForce(IDirectInputDevice8W& device) {
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    DIEFFECTINFOW info{};
    info.dwSize = sizeof(info);
    return SUCCEEDED(device.GetCapabilities(&caps)) && (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0 &&
        SUCCEEDED(device.GetEffectInfo(&info, GUID_ConstantForce));
}

std::array<bool, axisCount> availableForceAxes(IDirectInputDevice8W& device) {
    std::array<bool, axisCount> axes{};
    if (supportsConstantForce(device)) {
        for (std::size_t axis = 0; axis < axes.size(); ++axis)
            axes[axis] = forceActuator(device, axis);
    }
    return axes;
}

struct Enumeration {
    IDirectInput8W& input;
    std::vector<DeviceInfo> devices;
};

BOOL CALLBACK addDevice(const DIDEVICEINSTANCEW* instance, void* context) {
    auto& enumeration = *static_cast<Enumeration*>(context);
    const auto device = createDevice(enumeration.input, instance->guidInstance);
    if (device && SUCCEEDED(device->SetDataFormat(&c_dfDIJoystick2))) {
        enumeration.devices.push_back({
            deviceId(instance->guidInstance),
            utf8(instance->tszInstanceName[0] ? instance->tszInstanceName : instance->tszProductName),
            availableAxes(*device), availableButtons(*device), availablePovs(*device), availableForceAxes(*device),
            utf8(instance->tszProductName)
        });
    }
    return DIENUM_CONTINUE;
}

InputPtr createDirectInput(DirectInputDiagnostic diagnostic) {
    static const char moduleAnchor = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) {
        if (diagnostic)
            diagnostic({}, "GetModuleHandleExW", HRESULT_FROM_WIN32(GetLastError()), false);
        return {};
    }
    void* input = nullptr;
    const auto result = DirectInput8Create(module, DIRECTINPUT_VERSION, IID_IDirectInput8W, &input, nullptr);
    InputPtr instance(SUCCEEDED(result) ? static_cast<IDirectInput8W*>(input) : nullptr);
    if (diagnostic)
        diagnostic({}, "DirectInput8Create", result, instance != nullptr);
    return instance;
}

struct OpenDevice {
    ~OpenDevice() { release(); }

    void release() {
        if (device) {
            device->Unacquire();
            device.reset();
        }
        acquired = false;
        continuity.reset();
        axes = {};
        povs = {};
        buttons = {};
        ++layoutConnection;
    }

    void report(DirectInputDiagnostic diagnostic, std::string_view id, std::string_view stage,
        HRESULT result, bool available) {
        if (!diagnostic || (lastAvailable == available && (available || (lastStage == stage && lastResult == result))))
            return;
        lastAvailable = available;
        lastStage = stage;
        lastResult = result;
        diagnostic(id, stage, result, available);
    }

    DevicePtr device;
    DeviceObjectProbe<axisCount> axes;
    DeviceObjectProbe<povCount> povs;
    DeviceObjectProbe<buttonCount> buttons;
    bool acquired{};
    Clock::time_point retryAfter{};
    DeviceReadRecovery recovery;
    DeviceReadContinuity continuity;
    DeviceReadStatus readStatus{DeviceReadStatus::unavailable};
    std::string_view lastStage;
    HRESULT lastResult{};
    bool lastAvailable{};
    std::uint64_t sampleCycle{};
    std::optional<DeviceSample> cachedSample;
    struct ReadFault {
        DirectInputReadEventKind kind;
        std::string_view operation;
        HRESULT result;
    };
    std::optional<ReadFault> reportedFault;
    Clock::time_point lastSlowReport{};
    bool reportedReadTimeout{};
    std::uint64_t recoveryToken{};
    std::uint64_t layoutConnection{};
};

class ReadObserver {
public:
    ReadObserver(OpenDevice& device, std::string_view id, DirectInputReadObserver callback, void* context) noexcept
        : device_(device), id_(id), callback_(callback), context_(context) {}

    template <class Call>
    HRESULT measure(std::string_view operation, Call call) noexcept(noexcept(call())) {
        if (!callback_)
            return call();
        const auto started = Clock::now();
        const auto result = call();
        const auto now = Clock::now();
        const auto duration = now - started;
        micros_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
        const bool timedOut = duration > pendingGrace;
        if (duration >= slowInputOperation &&
            (device_.lastSlowReport == Clock::time_point{} || now - device_.lastSlowReport >= slowInputReportInterval
             || (timedOut && !device_.reportedReadTimeout))) {
            device_.lastSlowReport = now;
            device_.reportedReadTimeout = timedOut;
            report(DirectInputReadEventKind::slow, operation, result);
        }
        return result;
    }

    void fault(DirectInputReadEventKind kind, std::string_view operation, HRESULT result) const noexcept {
        if (!callback_)
            return;
        const auto& previous = device_.reportedFault;
        if (previous && previous->kind == kind &&
            (kind == DirectInputReadEventKind::pending || (previous->operation == operation && previous->result == result))) {
            return;
        }
        device_.reportedFault = OpenDevice::ReadFault{kind, operation, result};
        report(kind, operation, result);
    }

    void recovered(std::string_view operation, HRESULT result) const noexcept {
        if (!device_.reportedFault)
            return;
        device_.reportedFault.reset();
        report(DirectInputReadEventKind::recovered, operation, result);
    }

private:
    void report(DirectInputReadEventKind kind, std::string_view operation, HRESULT result) const noexcept {
        callback_(context_, {kind, id_, operation, result, micros_});
    }

    OpenDevice& device_;
    std::string_view id_;
    DirectInputReadObserver callback_;
    void* context_;
    std::uint64_t micros_{};
};

template <std::size_t Count, class Offset>
bool refreshObjects(DeviceObjectProbe<Count>& layout, const std::bitset<Count>& wanted,
                    IDirectInputDevice8W& device, ReadObserver& observer, std::string_view operation,
                    DWORD type, Offset offset) {
    bool needsAcquisition = false;
    layout.refresh(wanted, Clock::now(), [&](std::size_t index) {
        bool present = false;
        const auto result = observer.measure(operation, [&] {
            HRESULT status{};
            present = hasObject(device, offset(index), type, &status);
            return status;
        });
        // Some drivers defer metadata on an unacquired handle without returning DIERR_NOTACQUIRED.
        needsAcquisition |= result == DIERR_NOTACQUIRED || result == E_PENDING;
        return std::tuple{result, present, Clock::now()};
    });
    return needsAcquisition;
}

class LayoutDiscovery {
public:
    LayoutDiscovery(HWND window, DirectInputReadObserver observer, void* context) noexcept
        : window_(window), observer_(observer), context_(context) {}

    DeviceLayoutSnapshot operator()(const DeviceLayoutRequest& request) {
        if (!opened_ || id_ != request.id || connection_ != request.connection) {
            opened_ = std::make_unique<OpenDevice>();
            id_ = request.id;
            connection_ = request.connection;
            needsAcquisition_ = false;
        }
        auto& opened = *opened_;
        ReadObserver observer(opened, request.id, observer_, context_);
        if (!opened.device && !open(observer))
            return snapshot(request.wanted);

        if (needsAcquisition_) {
            const auto result = observer.measure("Probe Acquire", [&] { return opened.device->Acquire(); });
            if (FAILED(result))
                return snapshot(request.wanted);
            opened.acquired = true;
        }
        const auto& wanted = request.wanted;
        needsAcquisition_ |= refreshObjects(opened.axes, wanted.axes, *opened.device, observer,
            "GetObjectInfo axis", DIDFT_ABSAXIS, [](std::size_t index) { return axisOffsets[index]; });
        needsAcquisition_ |= refreshObjects(opened.povs, wanted.povs, *opened.device, observer,
            "GetObjectInfo POV", DIDFT_POV, [](std::size_t index) { return static_cast<DWORD>(DIJOFS_POV(index)); });
        needsAcquisition_ |= refreshObjects(opened.buttons, wanted.buttons, *opened.device, observer,
            "GetObjectInfo button", DIDFT_BUTTON, [](std::size_t index) { return DIJOFS_BUTTON(static_cast<DWORD>(index)); });
        if (opened.acquired) {
            const auto result = observer.measure("Probe Unacquire", [&] { return opened.device->Unacquire(); });
            opened.acquired = false;
            if (FAILED(result))
                opened.device.reset();
        }
        return snapshot(wanted);
    }

private:
    bool open(ReadObserver& observer) {
        if (!input_)
            input_ = createDirectInput(nullptr);
        GUID guid{};
        if (!input_ || FAILED(parseDeviceId(id_, guid)))
            return false;
        DevicePtr device;
        const auto created = observer.measure("Probe CreateDevice", [&] {
            HRESULT result{};
            device = createDevice(*input_, guid, &result);
            return result;
        });
        if (FAILED(created) || !device)
            return false;
        if (FAILED(observer.measure("Probe SetDataFormat", [&] { return device->SetDataFormat(&c_dfDIJoystick2); })))
            return false;
        if (FAILED(observer.measure("Probe SetCooperativeLevel", [&] {
            return device->SetCooperativeLevel(window_, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        })))
            return false;
        opened_->device = std::move(device);
        return true;
    }

    DeviceObjectSelection pending(const DeviceObjectSelection& wanted) const noexcept {
        return {opened_->axes.pending() & wanted.axes, opened_->buttons.pending() & wanted.buttons,
                opened_->povs.pending() & wanted.povs};
    }

    DeviceLayoutSnapshot snapshot(const DeviceObjectSelection& wanted) const noexcept {
        return {{opened_->axes.present() & wanted.axes, opened_->buttons.present() & wanted.buttons,
                 opened_->povs.present() & wanted.povs}, pending(wanted)};
    }

    HWND window_;
    DirectInputReadObserver observer_;
    void* context_;
    InputPtr input_;
    std::unique_ptr<OpenDevice> opened_;
    std::string id_;
    std::uint64_t connection_{};
    bool needsAcquisition_{};
};

struct WheelOutput {
    WheelOutput(DirectInputReadObserver observer, void* context) noexcept
        : observer(observer), observerContext(context) {}

    ~WheelOutput() { close(); }

    template <class Call>
    HRESULT measure(std::string_view operation, Call&& call) const noexcept {
        if (!observer)
            return call();
        const auto start = Clock::now();
        const auto result = call();
        const auto elapsed = Clock::now() - start;
        if (elapsed >= slowInputOperation) {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            observer(observerContext, {DirectInputReadEventKind::slow, id, operation, result,
                static_cast<std::uint64_t>(micros)});
        }
        return result;
    }

    void close() noexcept {
        if (effect) {
            measure("FFB Stop", [&] { return effect->Stop(); });
            effect.reset();
        }
        playing = false;
        neutral = false;
        if (device) {
            if (acquired)
                measure("FFB Unacquire", [&] { return device->Unacquire(); });
            if (autocenter) {
                DIPROPDWORD property{{sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, *autocenter};
                measure("FFB RestoreAutoCenter", [&] { return device->SetProperty(DIPROP_AUTOCENTER, &property.diph); });
            }
            device.reset();
        }
        acquired = false;
        autocenter.reset();
    }

    void report(DirectInputDiagnostic diagnostic, std::string_view stage, HRESULT result, bool available) {
        if (diagnostic && (stage != lastStage || result != lastResult)) {
            lastStage = stage;
            lastResult = result;
            diagnostic(id, stage, result, available);
        }
    }

    DevicePtr device;
    EffectPtr effect;
    std::optional<DWORD> autocenter;
    std::string id;
    std::size_t axis{axisCount};
    WheelForceRetry retry;
    Clock::time_point submitted{};
    std::string_view lastStage;
    HRESULT lastResult{};
    long magnitude{};
    bool acquired{};
    bool playing{};
    bool neutral{};
    DirectInputReadObserver observer{};
    void* observerContext{};
};

} // namespace

DeviceReadStatus DeviceReadContinuity::pending(Clock::time_point now) noexcept {
    pending_ = true;
    return lastSampled_ != Clock::time_point{} && now - lastSampled_ < pendingGrace
        ? DeviceReadStatus::pending : DeviceReadStatus::unavailable;
}

DeviceReadStatus DeviceReadContinuity::available(Clock::time_point sampledAt) noexcept {
    const bool interrupted = pending_ &&
        (lastSampled_ == Clock::time_point{} || sampledAt - lastSampled_ >= pendingGrace);
    lastSampled_ = sampledAt;
    pending_ = false;
    return interrupted ? DeviceReadStatus::resumed : DeviceReadStatus::available;
}

void DeviceReadContinuity::reset() noexcept {
    *this = {};
}

struct DirectInput::Impl {
    Impl(HWND windowHandle, DirectInputDiagnostic callback, DirectInputReadObserver readObserver, void* observerContext)
        : window(windowHandle), diagnostic(callback), input(createDirectInput(callback)),
          readObserver(readObserver), observerContext(observerContext), wheel(readObserver, observerContext) {}

    std::optional<DeviceSample> read(OpenDevice& opened, const std::string& id,
                                     const DeviceObjectSelection& wanted, bool rawPovs = false);

    HWND window;
    DirectInputDiagnostic diagnostic;
    InputPtr input;
    DirectInputReadObserver readObserver;
    void* observerContext;
    std::unordered_map<std::string, OpenDevice> openDevices;
    std::uint64_t pollCycle{};
    WheelOutput wheel;
    std::unique_ptr<DeviceLayoutWorker<LayoutDiscovery>> drivingLayout;
};

const char* axisName(std::size_t index) noexcept {
    return index < axisNames.size() ? axisNames[index] : "?";
}

bool isDeviceId(std::string_view id) noexcept {
    if (id.size() != 38 || id.front() != '{' || id.back() != '}')
        return false;

    for (std::size_t i = 1; i < id.size() - 1; ++i) {
        const char c = id[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (c != '-')
                return false;
        } else if (std::string_view("0123456789ABCDEF").find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

DirectInput::DirectInput(HWND window, DirectInputDiagnostic diagnostic,
                         DirectInputReadObserver readObserver, void* observerContext)
    : impl_(std::make_unique<Impl>(window, diagnostic, readObserver, observerContext)) {}

DirectInput::~DirectInput() = default;

bool DirectInput::ready() const noexcept {
    return impl_->input != nullptr;
}

std::vector<DeviceInfo> DirectInput::devices() const {
    if (!ready()) {
        return {};
    }
    Enumeration enumeration{*impl_->input, {}};
    impl_->input->EnumDevices(DI8DEVCLASS_GAMECTRL, addDevice, &enumeration, DIEDFL_ATTACHEDONLY);
    return std::move(enumeration.devices);
}

void DirectInput::beginPoll() noexcept {
    ++impl_->pollCycle;
}

std::optional<DeviceSample> DirectInput::Impl::read(OpenDevice& opened, const std::string& id,
                                                    const DeviceObjectSelection& wanted, bool rawPovs) {
    opened.readStatus = DeviceReadStatus::unavailable;
    if (opened.retryAfter != Clock::time_point{} && Clock::now() < opened.retryAfter) {
        return std::nullopt;
    }
    opened.retryAfter = {};
    ReadObserver observer(opened, id, readObserver, observerContext);
    const auto retry = [&](std::string_view stage, HRESULT result) {
        opened.recovery.failed((stage == "GetDeviceStatus" && result == DI_NOTATTACHED) ||
            result == static_cast<HRESULT>(DIERR_UNPLUGGED) || result == DIERR_INPUTLOST);
        opened.continuity.reset();
        opened.retryAfter = Clock::now() + DeviceReadRecovery::reopenDelay;
        opened.report(diagnostic, id, stage, result, false);
        observer.fault(DirectInputReadEventKind::unavailable, stage, result);
        return std::nullopt;
    };
    if (!opened.device) {
        GUID guid{};
        auto result = parseDeviceId(id, guid);
        if (FAILED(result))
            return retry("CLSIDFromString", result);
        result = observer.measure("GetDeviceStatus", [&] { return input->GetDeviceStatus(guid); });
        if (result != DI_OK)
            return retry("GetDeviceStatus", result);
        DevicePtr device;
        result = observer.measure("CreateDevice", [&] {
            HRESULT createResult{};
            device = createDevice(*input, guid, &createResult);
            return createResult;
        });
        if (!device)
            return retry("CreateDevice", result);
        result = observer.measure("SetDataFormat", [&] { return device->SetDataFormat(&c_dfDIJoystick2); });
        if (FAILED(result))
            return retry("SetDataFormat", result);
        result = observer.measure("SetCooperativeLevel", [&] {
            return device->SetCooperativeLevel(window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        });
        if (FAILED(result))
            return retry("SetCooperativeLevel", result);
        opened.device = std::move(device);
    }
    if (!opened.acquired) {
        const auto result = observer.measure("Acquire", [&] { return opened.device->Acquire(); });
        if (FAILED(result)) {
            if (result != DIERR_OTHERAPPHASPRIO)
                opened.release();
            return retry("Acquire", result);
        }
        opened.acquired = true;
    }

    DIJOYSTATE2 state{};
    const auto sampledAt = Clock::now();
    std::string_view stage = "Poll";
    // Devices that do not require polling return DI_NOEFFECT, which is a successful HRESULT.
    auto result = observer.measure(stage, [&] { return opened.device->Poll(); });
    if (SUCCEEDED(result)) {
        stage = "GetDeviceState";
        result = observer.measure(stage, [&] { return opened.device->GetDeviceState(sizeof(state), &state); });
    }
    if (result == E_PENDING) {
        const auto now = Clock::now();
        const auto pending = opened.recovery.pending(now);
        if (pending.first)
            observer.fault(DirectInputReadEventKind::pending, stage, result);
        if (pending.reopen) {
            // A device that never supplies a fresh report needs a new DirectInput handle.
            opened.release();
            return retry(stage, result);
        }
        opened.readStatus = opened.continuity.pending(now);
        if (opened.readStatus == DeviceReadStatus::unavailable && pending.elapsed >= pendingGrace)
            observer.fault(DirectInputReadEventKind::unavailable, stage, result);
        return std::nullopt;
    }
    if (FAILED(result)) {
        // Reopen and check the layout again after a connection loss.
        opened.release();
        return retry(stage, result);
    }
    opened.readStatus = opened.continuity.available(sampledAt);
    if (opened.recovery.accepted()) {
        // Confirm a connection only when the driver provides a valid report again.
        opened.recoveryToken = hardwareRecoverySerial.fetch_add(1, std::memory_order_relaxed) + 1;
        opened.readStatus = DeviceReadStatus::resumed;
    }
    opened.report(diagnostic, id, stage, result, true);
    observer.recovered(stage, result);

    refreshObjects(opened.axes, wanted.axes, *opened.device, observer,
        "GetObjectInfo axis", DIDFT_ABSAXIS, [](std::size_t index) { return axisOffsets[index]; });
    refreshObjects(opened.povs, wanted.povs, *opened.device, observer,
        "GetObjectInfo POV", DIDFT_POV, [](std::size_t index) { return static_cast<DWORD>(DIJOFS_POV(index)); });

    DeviceSample sample;
    sample.axes = {state.lX, state.lY, state.lZ, state.lRx, state.lRy, state.lRz,
        state.rglSlider[0], state.rglSlider[1]};
    sample.axesPending = wanted.axes & opened.axes.pending();
    sample.povsPending = wanted.povs & opened.povs.pending();
    sample.sampledAt = sampledAt;
    sample.recoveryToken = opened.recoveryToken;
    for (std::size_t index = 0; index < axisCount; ++index)
        sample.axesPresent[index] = opened.axes.present()[index];
    for (std::size_t index = 0; index < buttonCount; ++index) {
        if ((state.rgbButtons[index] & 0x80) != 0)
            sample.buttons.set(index);
    }
    for (std::size_t index = 0; index < povCount; ++index) {
        sample.povsPresent[index] = opened.povs.present()[index];
        if ((rawPovs || sample.povsPresent[index]) && state.rgdwPOV[index] < 36000)
            sample.povs[index] = static_cast<std::uint16_t>(state.rgdwPOV[index]);
    }
    return sample;
}

std::optional<DeviceSample> DirectInput::sample(const std::string& id, const std::bitset<buttonCount>& buttons,
                                               DeviceReadStatus* status) {
    DeviceReadStatus readStatus{};
    auto result = sampleConfigured(id, {std::bitset<axisCount>{}.set(), buttons, std::bitset<povCount>{}.set()},
                                   readStatus);
    if (status)
        *status = readStatus;
    return result;
}

std::optional<DeviceSample> DirectInput::sampleConfigured(const std::string& id, const DeviceObjectSelection& wanted,
                                                         DeviceReadStatus& status) {
    if (!ready() || id.empty()) {
        status = DeviceReadStatus::unavailable;
        return std::nullopt;
    }

    auto& opened = impl_->openDevices[id];
    if (impl_->pollCycle == 0 || opened.sampleCycle != impl_->pollCycle) {
        opened.cachedSample = impl_->read(opened, id, wanted);
        opened.sampleCycle = impl_->pollCycle;
    }
    status = opened.readStatus;
    if (opened.cachedSample) {
        ReadObserver observer(opened, id, impl_->readObserver, impl_->observerContext);
        refreshObjects(opened.buttons, wanted.buttons, *opened.device, observer,
            "GetObjectInfo button", DIDFT_BUTTON, [](std::size_t index) { return DIJOFS_BUTTON(static_cast<DWORD>(index)); });
        opened.cachedSample->buttonsPresent = opened.buttons.present();
        opened.cachedSample->buttonsPending = wanted.buttons & opened.buttons.pending();
    }
    return opened.cachedSample;
}

std::optional<DeviceSample> DirectInput::sampleForDriving(const std::string& id, const DeviceObjectSelection& wanted,
                                                         DeviceReadStatus& status) {
    if (!ready() || id.empty()) {
        status = DeviceReadStatus::unavailable;
        return std::nullopt;
    }
    auto& opened = impl_->openDevices[id];
    auto sample = impl_->read(opened, id, {}, true);
    status = opened.readStatus;
    if (!sample)
        return std::nullopt;

    if (!impl_->drivingLayout)
        impl_->drivingLayout = std::make_unique<DeviceLayoutWorker<LayoutDiscovery>>(
            LayoutDiscovery(impl_->window, impl_->readObserver, impl_->observerContext));
    const auto layout = impl_->drivingLayout->read(id, wanted, opened.layoutConnection);
    sample->axesPending = layout.pending.axes;
    sample->buttonsPending = layout.pending.buttons;
    sample->povsPending = layout.pending.povs;
    sample->buttonsPresent = layout.present.buttons;
    for (std::size_t axis = 0; axis < axisCount; ++axis)
        sample->axesPresent[axis] = layout.present.axes[axis];
    for (std::size_t pov = 0; pov < povCount; ++pov) {
        sample->povsPresent[pov] = layout.present.povs[pov];
        if (!sample->povsPresent[pov])
            sample->povs[pov] = povCentered;
    }
    return sample;
}

std::optional<float> DirectInput::setWheelForce(std::string_view id, std::size_t axis, float force,
    WheelForceGuard guard) {
    const auto started = Clock::now();
    const auto allowed = [&](WheelForceGuard::Check check) {
        return GetForegroundWindow() == impl_->window && (!check || check(guard.context));
    };
    const auto mayInitialize = guard.canInitialize ? guard.canInitialize : guard.current;
    if (!ready() || id.empty() || axis >= axisCount || !std::isfinite(force) || !allowed(guard.current))
        return std::nullopt;
    auto& wheel = impl_->wheel;
    const bool changedTarget = wheel.id != id || wheel.axis != axis;
    if (changedTarget)
        wheel.retry = {};
    const bool recovered = wheel.retry.recovered(guard.recoveryToken);
    if (changedTarget || recovered) {
        wheel.close();
        wheel.id = id;
        wheel.axis = axis;
        wheel.lastStage = {};
    }
    const auto magnitude = std::lround(std::clamp(force, -1.0f, 1.0f) * DI_FFNOMINALMAX);
    wheel.neutral = false;
    bool cancelled = false;
    const auto fail = [&](std::string_view stage, HRESULT result) {
        wheel.close();
        if (cancelled)
            return;
        const bool temporaryAccess = result == DIERR_OTHERAPPHASPRIO || result == DIERR_NOTACQUIRED ||
            result == DIERR_INPUTLOST || result == E_PENDING;
        wheel.retry.failed(Clock::now(), temporaryAccess);
        wheel.report(impl_->diagnostic, stage, result, false);
    };
    if (std::abs(magnitude) < 10) {
        if (wheel.effect && wheel.playing) {
            const auto result = wheel.measure("FFB Stop", [&] { return wheel.effect->Stop(); });
            if (!allowed(mayInitialize)) {
                wheel.close();
                return std::nullopt;
            }
            if (FAILED(result)) {
                fail("force feedback Stop", result);
                return std::nullopt;
            }
            wheel.playing = false;
        }
        wheel.neutral = true;
        return std::nullopt;
    }
    if (wheel.playing && wheel.magnitude == magnitude && started - wheel.submitted < forceRefresh)
        return std::nullopt;

    DICONSTANTFORCE constant{magnitude};
    DIEFFECT parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.cbTypeSpecificParams = sizeof(constant);
    parameters.lpvTypeSpecificParams = &constant;
    if (!wheel.effect) {
        if (wheel.retry.waiting(Clock::now()))
            return std::nullopt;
        const auto initialize = [&](std::string_view operation, auto&& call) -> HRESULT {
            if (cancelled || !allowed(mayInitialize)) {
                cancelled = true;
                return E_ABORT;
            }
            const auto result = wheel.measure(operation, call);
            cancelled = !allowed(mayInitialize);
            return cancelled ? E_ABORT : result;
        };
        GUID guid{};
        auto result = parseDeviceId(id, guid);
        if (FAILED(result)) {
            fail("force feedback GUID", result);
            return std::nullopt;
        }
        result = initialize("FFB CreateDevice", [&] {
            HRESULT status{};
            wheel.device = createDevice(*impl_->input, guid, &status);
            return status;
        });
        if (FAILED(result) || !wheel.device) {
            fail("force feedback CreateDevice", result);
            return std::nullopt;
        }
        result = initialize("FFB SetDataFormat", [&] { return wheel.device->SetDataFormat(&c_dfDIJoystick2); });
        if (FAILED(result)) {
            fail("force feedback SetDataFormat", result);
            return std::nullopt;
        }
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        result = initialize("FFB GetCapabilities", [&] { return wheel.device->GetCapabilities(&caps); });
        if (FAILED(result) || !(caps.dwFlags & DIDC_FORCEFEEDBACK)) {
            fail("force feedback GetCapabilities", FAILED(result) ? result : DIERR_UNSUPPORTED);
            return std::nullopt;
        }
        DIEFFECTINFOW info{};
        info.dwSize = sizeof(info);
        result = initialize("FFB GetEffectInfo", [&] {
            return wheel.device->GetEffectInfo(&info, GUID_ConstantForce);
        });
        if (FAILED(result)) {
            fail("force feedback GetEffectInfo", result);
            return std::nullopt;
        }
        DIDEVICEOBJECTINSTANCEW actuator{};
        actuator.dwSize = sizeof(actuator);
        result = initialize("FFB GetObjectInfo", [&] {
            return wheel.device->GetObjectInfo(&actuator, axisOffsets[axis], DIPH_BYOFFSET);
        });
        if (FAILED(result) || !(actuator.dwFlags & DIDOI_FFACTUATOR) || !(actuator.dwType & DIDFT_ABSAXIS)) {
            fail("force feedback actuator", FAILED(result) ? result : DIERR_UNSUPPORTED);
            return std::nullopt;
        }
        result = initialize("FFB SetCooperativeLevel", [&] {
            return wheel.device->SetCooperativeLevel(impl_->window, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
        });
        if (FAILED(result)) {
            fail("force feedback SetCooperativeLevel", result);
            return std::nullopt;
        }
        DIPROPDWORD property{{sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, 0};
        if (SUCCEEDED(initialize("FFB GetAutoCenter", [&] {
                return wheel.device->GetProperty(DIPROP_AUTOCENTER, &property.diph);
            }))) {
            const auto original = property.dwData;
            property.dwData = DIPROPAUTOCENTER_OFF;
            initialize("FFB SetAutoCenter", [&] {
                const auto changed = wheel.device->SetProperty(DIPROP_AUTOCENTER, &property.diph);
                if (SUCCEEDED(changed))
                    wheel.autocenter = original;
                return changed;
            });
        }
        result = initialize("FFB Acquire", [&] {
            const auto acquired = wheel.device->Acquire();
            wheel.acquired = SUCCEEDED(acquired);
            return acquired;
        });
        if (FAILED(result)) {
            fail("force feedback Acquire", result);
            return std::nullopt;
        }

        DWORD offset = axisOffsets[axis];
        LONG direction = 0;
        parameters.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        // A stalled worker cannot leave a persistent force.
        parameters.dwDuration = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::microseconds>(forceDuration).count());
        parameters.dwGain = DI_FFNOMINALMAX;
        parameters.dwTriggerButton = DIEB_NOTRIGGER;
        parameters.cAxes = 1;
        parameters.rgdwAxes = &offset;
        parameters.rglDirection = &direction;
        IDirectInputEffect* effect = nullptr;
        result = initialize("FFB CreateEffect", [&] {
            return wheel.device->CreateEffect(GUID_ConstantForce, &parameters, &effect, nullptr);
        });
        EffectPtr initialized(effect);
        if (FAILED(result) || !initialized) {
            initialized.reset();
            fail("force feedback CreateEffect", FAILED(result) ? result : E_FAIL);
            return std::nullopt;
        }
        wheel.effect = std::move(initialized);
    }

    // Opening an effect does not play it. Keep a successfully initialized effect
    // for the next fresh request if initialization outlasted this sample.
    if (!allowed(guard.current)) {
        if (!allowed(mayInitialize))
            wheel.close();
        return std::nullopt;
    }
    const auto submitted = Clock::now();
    const auto result = wheel.measure("FFB SetParameters", [&] {
        return wheel.effect->SetParameters(&parameters, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    });
    if (FAILED(result) || result == DI_DOWNLOADSKIPPED) {
        cancelled = !allowed(mayInitialize);
        fail("force feedback SetParameters", result);
        return std::nullopt;
    }
    if (!allowed(guard.current)) {
        stopWheelForce();
        return std::nullopt;
    }
    wheel.playing = true;
    wheel.magnitude = magnitude;
    wheel.submitted = submitted;
    wheel.retry.succeeded();
    wheel.report(impl_->diagnostic, "force feedback active", DI_OK, true);
    return static_cast<float>(magnitude) / DI_FFNOMINALMAX;
}

WheelForceState DirectInput::wheelForceState() const noexcept {
    if (!ready())
        return WheelForceState::unavailable;
    const auto& wheel = impl_->wheel;
    const auto now = Clock::now();
    if (wheel.playing && now - wheel.submitted < forceDuration)
        return WheelForceState::active;
    if (wheel.retry.waiting(now))
        return WheelForceState::unavailable;
    return wheel.neutral ? WheelForceState::neutral : WheelForceState::waiting;
}

void DirectInput::stopWheelForce() noexcept {
    impl_->wheel.close();
}

void DirectInput::close() {
    impl_->drivingLayout.reset();
    stopWheelForce();
    impl_->openDevices.clear();
}

} // namespace chrome_axis
