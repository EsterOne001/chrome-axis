#define DIRECTINPUT_VERSION 0x0800

#include <chrome_axis/input.hpp>
#include <chrome_axis/feedback.hpp>

#include <dinput.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace chrome_axis {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto reconnectDelay = std::chrono::milliseconds(500);
constexpr auto forceDuration = std::chrono::milliseconds(100);
constexpr auto forceRefresh = std::chrono::milliseconds(60);
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

HRESULT parseDeviceId(const std::string& id, GUID& guid) {
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

bool hasObject(IDirectInputDevice8W& device, DWORD offset, DWORD type) {
    DIDEVICEOBJECTINSTANCEW object{};
    object.dwSize = sizeof(object);
    return SUCCEEDED(device.GetObjectInfo(&object, offset, DIPH_BYOFFSET))
        && (object.dwType & type) != 0;
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

std::array<bool, axisCount> availableForceAxes(IDirectInputDevice8W& device) {
    std::array<bool, axisCount> axes{};
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    DIEFFECTINFOW info{};
    info.dwSize = sizeof(info);
    if (SUCCEEDED(device.GetCapabilities(&caps)) && (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0 &&
        SUCCEEDED(device.GetEffectInfo(&info, GUID_ConstantForce))) {
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
            availableAxes(*device), availableButtons(*device), availablePovs(*device), availableForceAxes(*device)
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
        buttonsChecked.reset();
        buttonsPresent.reset();
    }

    HRESULT read(DIJOYSTATE2& state, std::string_view& stage) const {
        if (needsPolling) {
            const auto result = device->Poll();
            if (FAILED(result)) {
                stage = "Poll";
                return result;
            }
        }
        stage = "GetDeviceState";
        return device->GetDeviceState(sizeof(state), &state);
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
    std::array<bool, axisCount> axes{};
    std::array<bool, povCount> povs{};
    std::bitset<buttonCount> buttonsChecked;
    std::bitset<buttonCount> buttonsPresent;
    bool needsPolling{true};
    bool acquired{};
    Clock::time_point retryAfter{};
    Clock::time_point pendingSince{};
    std::string_view lastStage;
    HRESULT lastResult{};
    bool lastAvailable{};
    std::uint64_t sampleCycle{};
    std::optional<DeviceSample> cachedSample;
};

struct WheelOutput {
    ~WheelOutput() { close(); }

    void close() noexcept {
        if (effect) {
            effect->Stop();
            effect.reset();
        }
        playing = false;
        if (device) {
            device->Unacquire();
            if (autocenter) {
                DIPROPDWORD property{{sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, *autocenter};
                device->SetProperty(DIPROP_AUTOCENTER, &property.diph);
            }
            device.reset();
        }
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
    Clock::time_point retryAfter{};
    Clock::time_point submitted{};
    std::string_view lastStage;
    HRESULT lastResult{};
    long magnitude{};
    bool playing{};
};

} // namespace

struct DirectInput::Impl {
    Impl(HWND windowHandle, DirectInputDiagnostic callback)
        : window(windowHandle), diagnostic(callback), input(createDirectInput(callback)) {}

    std::optional<DeviceSample> read(OpenDevice& opened, const std::string& id);

    HWND window;
    DirectInputDiagnostic diagnostic;
    InputPtr input;
    std::unordered_map<std::string, OpenDevice> openDevices;
    std::uint64_t pollCycle{};
    WheelOutput wheel;
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

DirectInput::DirectInput(HWND window, DirectInputDiagnostic diagnostic)
    : impl_(std::make_unique<Impl>(window, diagnostic)) {}

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

std::optional<DeviceSample> DirectInput::Impl::read(OpenDevice& opened, const std::string& id) {
    if (opened.retryAfter != Clock::time_point{} && Clock::now() < opened.retryAfter) {
        return std::nullopt;
    }
    opened.retryAfter = {};
    const auto retry = [&](std::string_view stage, HRESULT result) {
        opened.pendingSince = {};
        opened.retryAfter = Clock::now() + reconnectDelay;
        opened.report(diagnostic, id, stage, result, false);
        return std::nullopt;
    };
    if (!opened.device) {
        GUID guid{};
        auto result = parseDeviceId(id, guid);
        if (FAILED(result))
            return retry("CLSIDFromString", result);
        result = input->GetDeviceStatus(guid);
        if (result != DI_OK)
            return retry("GetDeviceStatus", result);
        auto device = createDevice(*input, guid, &result);
        if (!device)
            return retry("CreateDevice", result);
        result = device->SetDataFormat(&c_dfDIJoystick2);
        if (FAILED(result))
            return retry("SetDataFormat", result);
        result = device->SetCooperativeLevel(window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
        if (FAILED(result))
            return retry("SetCooperativeLevel", result);
        opened.axes = availableAxes(*device);
        opened.povs = availablePovs(*device);
        DIDEVCAPS capabilities{};
        capabilities.dwSize = sizeof(capabilities);
        opened.needsPolling = FAILED(device->GetCapabilities(&capabilities))
            || (capabilities.dwFlags & DIDC_POLLEDDATAFORMAT) != 0;
        opened.device = std::move(device);
    }
    if (!opened.acquired) {
        const auto result = opened.device->Acquire();
        if (FAILED(result)) {
            if (result != DIERR_OTHERAPPHASPRIO)
                opened.release();
            return retry("Acquire", result);
        }
        opened.acquired = true;
    }

    DIJOYSTATE2 state{};
    std::string_view stage;
    const auto result = opened.read(state, stage);
    if (result == E_PENDING) {
        if (diagnostic) {
            const auto now = Clock::now();
            if (opened.pendingSince == Clock::time_point{})
                opened.pendingSince = now;
            if (now - opened.pendingSince >= reconnectDelay)
                opened.report(diagnostic, id, stage, result, false);
        }
        return std::nullopt;
    }
    if (FAILED(result)) {
        // Reopen and check the layout again after a connection loss.
        opened.release();
        return retry(stage, result);
    }
    opened.pendingSince = {};
    opened.report(diagnostic, id, stage, result, true);

    DeviceSample sample;
    sample.axes = {state.lX, state.lY, state.lZ, state.lRx, state.lRy, state.lRz,
        state.rglSlider[0], state.rglSlider[1]};
    sample.axesPresent = opened.axes;
    sample.povsPresent = opened.povs;
    for (std::size_t index = 0; index < buttonCount; ++index) {
        if ((state.rgbButtons[index] & 0x80) != 0)
            sample.buttons.set(index);
    }
    for (std::size_t index = 0; index < povCount; ++index) {
        if (opened.povs[index] && state.rgdwPOV[index] < 36000)
            sample.povs[index] = static_cast<std::uint16_t>(state.rgdwPOV[index]);
    }
    return sample;
}

std::optional<DeviceSample> DirectInput::sample(const std::string& id, const std::bitset<buttonCount>& buttons) {
    if (!ready() || id.empty())
        return std::nullopt;

    auto& opened = impl_->openDevices[id];
    if (impl_->pollCycle == 0 || opened.sampleCycle != impl_->pollCycle) {
        opened.cachedSample = impl_->read(opened, id);
        opened.sampleCycle = impl_->pollCycle;
    }
    if (opened.cachedSample) {
        const auto unchecked = buttons & ~opened.buttonsChecked;
        if (unchecked.any()) {
            for (DWORD index = 0; index < buttonCount; ++index) {
                if (unchecked.test(index))
                    opened.buttonsPresent[index] = hasObject(*opened.device, DIJOFS_BUTTON(index), DIDFT_BUTTON);
            }
            opened.buttonsChecked |= unchecked;
        }
        opened.cachedSample->buttonsPresent = opened.buttonsPresent;
    }
    return opened.cachedSample;
}

void DirectInput::setWheelForce(const std::string& id, std::size_t axis, float force,
                                bool (*stillCurrent)(const void*) noexcept, const void* context) {
    const auto started = Clock::now();
    const auto allowed = [&] {
        return GetForegroundWindow() == impl_->window && (!stillCurrent || stillCurrent(context));
    };
    if (!ready() || id.empty() || axis >= axisCount || !std::isfinite(force) || !allowed()) {
        stopWheelForce();
        return;
    }
    auto& wheel = impl_->wheel;
    if (wheel.id != id || wheel.axis != axis) {
        wheel.close();
        wheel.id = id;
        wheel.axis = axis;
        wheel.retryAfter = {};
        wheel.lastStage = {};
    }
    constexpr float limit = static_cast<float>(maxFeedbackPercent) / 100.0f;
    const auto magnitude = std::lround(std::clamp(force, -limit, limit) * DI_FFNOMINALMAX);
    const auto fail = [&](std::string_view stage, HRESULT result) {
        wheel.close();
        wheel.retryAfter = Clock::now() + std::chrono::seconds(2);
        wheel.report(impl_->diagnostic, stage, result, false);
    };
    if (std::abs(magnitude) < 10) {
        if (wheel.effect && wheel.playing) {
            const auto result = wheel.effect->Stop();
            if (FAILED(result)) {
                fail("force feedback Stop", result);
                return;
            }
            wheel.playing = false;
        }
        return;
    }
    if (wheel.playing && wheel.magnitude == magnitude && started - wheel.submitted < forceRefresh)
        return;

    DICONSTANTFORCE constant{magnitude};
    DIEFFECT parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.cbTypeSpecificParams = sizeof(constant);
    parameters.lpvTypeSpecificParams = &constant;
    if (!wheel.effect) {
        if (Clock::now() < wheel.retryAfter)
            return;
        GUID guid{};
        auto result = parseDeviceId(id, guid);
        if (FAILED(result)) {
            fail("force feedback GUID", result);
            return;
        }
        wheel.device = createDevice(*impl_->input, guid, &result);
        if (!wheel.device) {
            fail("force feedback CreateDevice", result);
            return;
        }
        result = wheel.device->SetDataFormat(&c_dfDIJoystick2);
        if (FAILED(result) || !availableForceAxes(*wheel.device)[axis]) {
            fail("force feedback actuator", FAILED(result) ? result : DIERR_UNSUPPORTED);
            return;
        }
        result = wheel.device->SetCooperativeLevel(impl_->window, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
        if (FAILED(result)) {
            fail("force feedback SetCooperativeLevel", result);
            return;
        }
        DIPROPDWORD property{{sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, 0};
        if (SUCCEEDED(wheel.device->GetProperty(DIPROP_AUTOCENTER, &property.diph))) {
            const auto original = property.dwData;
            property.dwData = DIPROPAUTOCENTER_OFF;
            if (SUCCEEDED(wheel.device->SetProperty(DIPROP_AUTOCENTER, &property.diph)))
                wheel.autocenter = original;
        }
        result = wheel.device->Acquire();
        if (FAILED(result)) {
            fail("force feedback Acquire", result);
            return;
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
        result = wheel.device->CreateEffect(GUID_ConstantForce, &parameters, &effect, nullptr);
        if (FAILED(result) || !effect) {
            fail("force feedback CreateEffect", FAILED(result) ? result : E_FAIL);
            return;
        }
        wheel.effect.reset(effect);
    }

    if (Clock::now() - started > std::chrono::milliseconds(70)) {
        fail("force feedback driver timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        return;
    }
    if (!allowed()) {
        stopWheelForce();
        return;
    }
    const auto submitted = Clock::now();
    const auto result = wheel.effect->SetParameters(&parameters, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    if (FAILED(result) || result == DI_DOWNLOADSKIPPED) {
        fail("force feedback SetParameters", result);
        return;
    }
    if (!allowed()) {
        stopWheelForce();
        return;
    }
    wheel.playing = true;
    wheel.magnitude = magnitude;
    wheel.submitted = submitted;
    wheel.report(impl_->diagnostic, "force feedback active", DI_OK, true);
}

void DirectInput::stopWheelForce() noexcept {
    impl_->wheel.close();
}

void DirectInput::close() {
    stopWheelForce();
    impl_->openDevices.clear();
}

} // namespace chrome_axis
