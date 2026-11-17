#define DIRECTINPUT_VERSION 0x0800

#include "chrome_axis/input.hpp"

#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

using namespace chrome_axis;
using namespace std::chrono_literals;
using Clock = DeviceReadClock;

constexpr char wheelId[] = "{11111111-1111-1111-1111-111111111111}";
constexpr char pedalsId[] = "{22222222-2222-2222-2222-222222222222}";
constexpr GUID wheelGuid{0x11111111, 0x1111, 0x1111, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}};
constexpr GUID pedalsGuid{0x22222222, 0x2222, 0x2222, {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22}};

enum class ProbeFailure { pending, accessDenied, errorAfterAcquireDenial, genericOnDiscovery };

struct MockController {
    ProbeFailure axisProbeFailure{ProbeFailure::pending};
    std::atomic<bool> connected{true};
    std::atomic<bool> reportsPending{};
    std::atomic<bool> buttonPresent{true};
    std::atomic<bool> waitForProbe{};
    std::atomic<bool> probeStarted{};
    std::atomic<bool> delayedButtonReady{};
    std::atomic<unsigned> deniedAcquires{};
    std::atomic<unsigned> readerAxisChecks{};
    std::atomic<unsigned> delayedButtonChecks{};
    std::atomic<unsigned> capabilityChecks{};
    bool acquisitionRequired{true};
    bool feedback{};
    bool setupBlocked{};
    DWORD autocenter{DIPROPAUTOCENTER_ON};
    LONG force{};
    bool exclusivelyAcquired{};
    unsigned effects{};
    unsigned exclusiveAcquires{};
};

struct MockDriver {
    MockController wheel;
    MockController pedals;
    DWORD samplingThread{GetCurrentThreadId()};
    HRESULT factoryResult{DI_OK};
};

MockDriver* driver{};

class MockEffect final : public IDirectInputEffect {
public:
    explicit MockEffect(MockController& controller) : controller_(controller) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining)
            delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT parameters, DWORD flags) override {
        if (flags != (DIEP_TYPESPECIFICPARAMS | DIEP_START) ||
            parameters->cbTypeSpecificParams != sizeof(DICONSTANTFORCE))
            return DIERR_INVALIDPARAM;
        controller_.force = static_cast<const DICONSTANTFORCE*>(parameters->lpvTypeSpecificParams)->lMagnitude;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Start(DWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stop() override {
        controller_.force = 0;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Download() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Unload() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return E_NOTIMPL; }

private:
    MockController& controller_;
    ULONG references_{1};
};

class MockDevice final : public IDirectInputDevice8W {
public:
    MockDevice(MockController& controller, bool discovery)
        : controller_(controller), discovery_(discovery) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining)
            delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        ++controller_.capabilityChecks;
        caps->dwFlags = controller_.feedback ? DIDC_FORCEFEEDBACK : 0;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKW, LPVOID, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID property, LPDIPROPHEADER value) override {
        if (!exclusive_ || &property != &DIPROP_AUTOCENTER)
            return E_NOTIMPL;
        reinterpret_cast<DIPROPDWORD*>(value)->dwData = controller_.autocenter;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID property, LPCDIPROPHEADER value) override {
        if (!exclusive_ || acquired_ || &property != &DIPROP_AUTOCENTER)
            return E_NOTIMPL;
        controller_.autocenter = reinterpret_cast<const DIPROPDWORD*>(value)->dwData;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Acquire() override {
        if (!controller_.connected)
            return DIERR_INPUTLOST;
        if (discovery_ && controller_.acquisitionRequired) {
            ++controller_.deniedAcquires;
            return DIERR_OTHERAPPHASPRIO;
        }
        acquired_ = true;
        if (exclusive_) {
            controller_.exclusivelyAcquired = true;
            ++controller_.exclusiveAcquires;
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Unacquire() override {
        acquired_ = false;
        if (exclusive_)
            controller_.exclusivelyAcquired = false;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID output) override {
        if (!controller_.connected)
            return DIERR_INPUTLOST;
        if (!acquired_)
            return DIERR_NOTACQUIRED;
        if (controller_.reportsPending)
            return E_PENDING;
        if (size != sizeof(DIJOYSTATE2))
            return DIERR_INVALIDPARAM;

        auto& state = *static_cast<DIJOYSTATE2*>(output);
        state = {};
        state.lX = 630;
        state.rgbButtons[0] = 0x80;
        std::ranges::fill(state.rgdwPOV, 0xffffffffu);
        state.rgdwPOV[0] = 9000;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD flags) override {
        exclusive_ = (flags & DISCL_EXCLUSIVE) != 0;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEW object, DWORD offset, DWORD how) override {
        if (how != DIPH_BYOFFSET)
            return DIERR_INVALIDPARAM;
        if (!controller_.connected)
            return DIERR_INPUTLOST;
        if (offset == DIJOFS_X) {
            if (!discovery_)
                ++controller_.readerAxisChecks;
            if (discovery_ && controller_.waitForProbe) {
                controller_.probeStarted = true;
                controller_.waitForProbe.wait(true);
            }
            if (!acquired_ && controller_.acquisitionRequired) {
                if (controller_.axisProbeFailure == ProbeFailure::accessDenied)
                    return DIERR_OTHERAPPHASPRIO;
                if (controller_.axisProbeFailure == ProbeFailure::genericOnDiscovery ||
                    (controller_.axisProbeFailure == ProbeFailure::errorAfterAcquireDenial && controller_.deniedAcquires))
                    return E_FAIL;
                return E_PENDING;
            }
            object->dwType = DIDFT_ABSAXIS;
            object->dwFlags = controller_.feedback ? DIDOI_FFACTUATOR : 0;
            return DI_OK;
        }
        if (offset == DIJOFS_POV(0)) {
            if (!acquired_ && controller_.acquisitionRequired)
                return E_PENDING;
            object->dwType = DIDFT_POV;
            return DI_OK;
        }
        if (offset == DIJOFS_BUTTON(0) || offset == DIJOFS_BUTTON(1) || offset == DIJOFS_BUTTON(2)) {
            if (!acquired_ && controller_.acquisitionRequired)
                return DIERR_NOTACQUIRED;
            if (offset == DIJOFS_BUTTON(1) || (offset == DIJOFS_BUTTON(0) && !controller_.buttonPresent))
                return DIERR_OBJECTNOTFOUND;
            if (offset == DIJOFS_BUTTON(2)) {
                ++controller_.delayedButtonChecks;
                if (!controller_.delayedButtonReady)
                    return E_PENDING;
            }
            object->dwType = DIDFT_BUTTON;
            return DI_OK;
        }
        return DIERR_OBJECTNOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEW) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID effect, LPCDIEFFECT parameters,
                                           LPDIRECTINPUTEFFECT* output, LPUNKNOWN) override {
        if (!controller_.feedback || !exclusive_ || !acquired_ || effect != GUID_ConstantForce ||
            parameters->dwDuration != 100000 || parameters->cAxes != 1 || parameters->rgdwAxes[0] != DIJOFS_X)
            return DIERR_INVALIDPARAM;
        ++controller_.effects;
        *output = new MockEffect(controller_);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKW, LPVOID, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOW, REFGUID effect) override {
        return controller_.feedback && effect == GUID_ConstantForce ? DI_OK : DIERR_UNSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Poll() override { return DI_NOEFFECT; }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCWSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCWSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATW, LPCWSTR, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATW, LPCWSTR, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERW) override { return E_NOTIMPL; }

private:
    MockController& controller_;
    ULONG references_{1};
    const bool discovery_;
    bool acquired_{};
    bool exclusive_{};
};

class MockInput final : public IDirectInput8W {
public:
    explicit MockInput(MockDriver& mock) : mock_(mock) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining)
            delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID id, LPDIRECTINPUTDEVICE8W* device, LPUNKNOWN) override {
        auto* const controller = find(id);
        if (!controller || !controller->connected)
            return DIERR_INPUTLOST;
        if (controller->setupBlocked)
            return DIERR_OTHERAPPHASPRIO;
        *device = new MockDevice(*controller, GetCurrentThreadId() != mock_.samplingThread);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKW callback, LPVOID context, DWORD) override {
        if (mock_.wheel.connected) {
            DIDEVICEINSTANCEW instance{};
            instance.dwSize = sizeof(instance);
            instance.guidInstance = wheelGuid;
            callback(&instance, context);
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID id) override {
        const auto* const controller = find(id);
        return controller && controller->connected ? DI_OK : DI_NOTATTACHED;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID, LPCWSTR, LPGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCWSTR, LPDIACTIONFORMATW, LPDIENUMDEVICESBYSEMANTICSCBW,
                                                       LPVOID, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSW, DWORD,
                                                LPVOID) override { return E_NOTIMPL; }

private:
    MockController* find(REFGUID id) const noexcept {
        if (id == wheelGuid)
            return &mock_.wheel;
        if (id == pedalsGuid)
            return &mock_.pedals;
        return nullptr;
    }

    MockDriver& mock_;
    ULONG references_{1};
};

template <class Predicate>
bool waitFor(Predicate&& predicate, Clock::duration timeout = 4s)
{
    const auto deadline = Clock::now() + timeout;
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    } while (Clock::now() < deadline);
    return false;
}

struct ReadEvents {
    std::atomic<bool> layoutPending{};
    std::atomic<bool> layoutAcquireDenied{};
    std::atomic<bool> layoutMissing{};
    std::atomic<bool> layoutRecovered{};
    std::atomic<bool> inputPending{};
    std::atomic<bool> inputLost{};

    static void observe(void* context, const DirectInputReadEvent& event) noexcept {
        auto& events = *static_cast<ReadEvents*>(context);
        if (event.deviceId != wheelId)
            return;
        switch (event.kind) {
        case DirectInputReadEventKind::layoutPending:
            if (event.result == E_PENDING || event.result == DIERR_NOTACQUIRED)
                events.layoutPending = true;
            break;
        case DirectInputReadEventKind::layoutAcquireDenied:
            if (event.result == DIERR_OTHERAPPHASPRIO)
                events.layoutAcquireDenied = true;
            break;
        case DirectInputReadEventKind::layoutMissing:
            if (event.result == DIERR_OBJECTNOTFOUND)
                events.layoutMissing = true;
            break;
        case DirectInputReadEventKind::layoutRecovered:
            events.layoutRecovered = true;
            break;
        case DirectInputReadEventKind::pending:
            if (event.result == E_PENDING)
                events.inputPending = true;
            break;
        case DirectInputReadEventKind::unavailable:
            if (event.result == DIERR_INPUTLOST)
                events.inputLost = true;
            break;
        default:
            break;
        }
    }
};

bool recoverRequiredLayout(MockController& wheel, DirectInput& input, const ReadEvents& events)
{
    DeviceReadStatus status{};
    DeviceObjectSelection wanted;
    wanted.axes.set(0);
    wanted.buttons.set(0).set(1);
    wanted.povs.set(0);
    bool sawPendingRawSignal = false;
    const auto read = [&] { return input.sampleForDriving(wheelId, wanted, status); };
    if (!waitFor([&] {
        const auto sample = read();
        if (!sample || status != DeviceReadStatus::available)
            return false;
        sawPendingRawSignal |= sample->axes[0] == 630 && sample->axesPending[0];
        return sample->axesPresent[0] && sample->buttonsPresent[0] && sample->povsPresent[0] &&
               !sample->buttonsPresent[1] && !sample->buttonsPending[1] && sample->axes[0] == 630 &&
               sample->buttons[0] && sample->povs[0] == 9000;
    }) || !sawPendingRawSignal || !wheel.deniedAcquires || wheel.capabilityChecks) {
        std::cerr << "Acquired input did not recover required layout after probe acquisition denial\n";
        return false;
    }

    if (!events.layoutPending || !events.layoutAcquireDenied || !events.layoutMissing || !events.layoutRecovered) {
        std::cerr << "Layout diagnostics did not distinguish pending, denied, missing and recovered metadata\n";
        return false;
    }

    const auto checks = wheel.readerAxisChecks.load();
    wanted.buttons.set(2);
    if (!waitFor([&] {
        const auto sample = read();
        return sample && wheel.delayedButtonChecks && sample->buttonsPending[2] &&
            !sample->buttonsPresent[2] && sample->axesPresent[0] && sample->buttonsPresent[0] &&
            sample->povsPresent[0] && !sample->buttonsPending[1];
    })) {
        std::cerr << "Changing selection should preserve already confirmed objects\n";
        return false;
    }

    // Keep feeding reads more frequently than the probe retry interval.
    const auto backoff = Clock::now() + 120ms;
    const auto delayedChecks = wheel.delayedButtonChecks.load();
    while (Clock::now() < backoff) {
        const auto sample = read();
        if (!sample || !sample->axesPresent[0] || !sample->buttonsPending[2])
            return false;
        std::this_thread::sleep_for(5ms);
    }
    if (wheel.readerAxisChecks != checks || wheel.delayedButtonChecks > delayedChecks + 1) {
        std::cerr << "Metadata work should not repeat on every sample\n";
        return false;
    }

    wheel.delayedButtonReady = true;
    if (!waitFor([&] {
        const auto sample = read();
        return sample && sample->buttonsPresent[2] && !sample->buttonsPending[2];
    })) {
        std::cerr << "A pending control was not retried\n";
        return false;
    }

    wheel.reportsPending = true;
    const auto pending = read();
    wheel.reportsPending = false;
    if (pending || status != DeviceReadStatus::pending || !events.inputPending) {
        std::cerr << "A missing physical report must not reuse the last input\n";
        return false;
    }

    wheel.connected = false;
    if (read() || status != DeviceReadStatus::unavailable || !events.inputLost) {
        std::cerr << "Lost device did not drop its sample\n";
        return false;
    }
    wheel.buttonPresent = false;
    wheel.connected = true;
    bool resumed = false;
    bool staleButton = false;
    if (!waitFor([&] {
        const auto sample = read();
        if (!sample)
            return false;
        resumed |= status == DeviceReadStatus::resumed && sample->recoveryToken != 0;
        if (sample->buttonsPresent[0]) {
            staleButton = true;
            return true;
        }
        return resumed && sample->axesPresent[0] && sample->povsPresent[0] &&
            sample->buttonsPresent[2] && !sample->buttonsPending[0];
    }) || !resumed || staleButton) {
        std::cerr << "Reconnected device did not publish its revised layout and token\n";
        return false;
    }
    return true;
}

bool recoverAfterMetadataError(ProbeFailure failure)
{
    MockDriver mock;
    mock.wheel.axisProbeFailure = failure;
    driver = &mock;
    DirectInput input(GetDesktopWindow());
    DeviceObjectSelection wanted;
    wanted.axes.set(0);
    DeviceReadStatus status{};
    bool sawPending = false;
    return waitFor([&] {
        const auto sample = input.sampleForDriving(wheelId, wanted, status);
        if (!sample || status != DeviceReadStatus::available)
            return false;
        sawPending |= sample->axesPending[0] && sample->axes[0] == 630;
        return sawPending && sample->axesPresent[0] && !sample->axesPending[0] &&
            sample->axes[0] == 630 && mock.wheel.deniedAcquires;
    }, 2s);
}

bool coldOpenAfterGenericProbeError()
{
    MockDriver mock;
    mock.wheel.acquisitionRequired = false;
    mock.pedals.acquisitionRequired = false;
    driver = &mock;
    const auto window = GetDesktopWindow();
    DeviceObjectSelection wanted;
    wanted.axes.set(0);
    DeviceReadStatus wheelStatus{}, pedalStatus{};
    {
        DirectInput firstReader(window);
        if (!waitFor([&] {
            const auto sample = firstReader.sampleForDriving(wheelId, wanted, wheelStatus);
            return sample && sample->axesPresent[0];
        }))
            return false;
    }

    mock.wheel.acquisitionRequired = true;
    mock.wheel.axisProbeFailure = ProbeFailure::genericOnDiscovery;
    DirectInput wheelReader(window);
    DirectInput pedalReader(window);
    if (!waitFor([&] {
        const auto sample = pedalReader.sampleForDriving(pedalsId, wanted, pedalStatus);
        return sample && sample->axesPresent[0];
    }))
        return false;

    bool sawPendingWheel = false;
    bool pedalInterrupted = false;
    const bool recovered = waitFor([&] {
        const auto pedal = pedalReader.sampleForDriving(pedalsId, wanted, pedalStatus);
        pedalInterrupted |= !pedal || !pedal->axesPresent[0] || pedalStatus != DeviceReadStatus::available;
        const auto wheel = wheelReader.sampleForDriving(wheelId, wanted, wheelStatus);
        if (!wheel || wheelStatus != DeviceReadStatus::available)
            return false;
        sawPendingWheel |= wheel->axesPending[0] && wheel->axes[0] == 630;
        return wheel->axesPresent[0] && !wheel->axesPending[0] && wheel->axes[0] == 630;
    }, 3s);
    if (recovered && sawPendingWheel && !pedalInterrupted && mock.wheel.readerAxisChecks)
        return true;
    std::cerr << "Cold reopen: wheel pending=" << sawPendingWheel << ", recovered=" << recovered
              << ", acquired reader probes=" << mock.wheel.readerAxisChecks.load()
              << ", pedals interrupted=" << pedalInterrupted << '\n';
    return false;
}

bool independentDiscovery(MockDriver& mock)
{
    MockController& wheel = mock.wheel;
    wheel.waitForProbe = true;
    DirectInput wheelInput(GetDesktopWindow());
    DirectInput pedalInput(GetDesktopWindow());
    struct UnblockProbe {
        MockController& wheel;
        ~UnblockProbe() {
            wheel.waitForProbe = false;
            wheel.waitForProbe.notify_one();
        }
    } unblock{wheel};

    DeviceObjectSelection wanted;
    wanted.axes.set(0);
    DeviceReadStatus status{};
    if (!wheelInput.sampleForDriving(wheelId, wanted, status) ||
        !waitFor([&] { return wheel.probeStarted.load(); }))
        return false;
    if (!waitFor([&] {
        const auto sample = pedalInput.sampleForDriving(pedalsId, wanted, status);
        return sample && sample->axesPresent[0] && sample->axes[0] == 630;
    })) {
        std::cerr << "A blocked wheel layout interrupted independent pedals\n";
        return false;
    }
    return true;
}

} // namespace

// Override only the factory in this test executable. The linked production reader
// still calls the same DirectInput interfaces on its real sampling/probe paths.
HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID* output, LPUNKNOWN)
{
    if (FAILED(driver->factoryResult)) {
        *output = nullptr;
        return driver->factoryResult;
    }
    *output = new MockInput(*driver);
    return S_OK;
}

namespace {

bool reportDirectInputInitializationFailure()
{
    MockDriver mock;
    mock.factoryResult = E_OUTOFMEMORY;
    driver = &mock;
    bool reported = false;
    DirectInput input(GetDesktopWindow(), nullptr, [](void* context, const DirectInputReadEvent& event) noexcept {
        *static_cast<bool*>(context) = event.kind == DirectInputReadEventKind::unavailable &&
            event.operation == "DirectInput8Create" && event.result == E_OUTOFMEMORY;
    }, &reported);
    return !input.ready() && reported;
}

HWND WINAPI mockForegroundWindow() {
    return reinterpret_cast<HWND>(1);
}

} // namespace

// Satisfy the imported Win32 symbol locally in this Windows x64 test executable.
// This leaves the production foreground check active without taking desktop focus.
extern "C" {
    decltype(&GetForegroundWindow) __imp_GetForegroundWindow = &mockForegroundWindow;
}

namespace {

bool forceWithDeferredActuator(bool invalidateDuringAcquire)
{
    MockDriver mock;
    mock.wheel.feedback = true;
    driver = &mock;
    DirectInput inactive(nullptr);
    if (inactive.setWheelForce(wheelId, 0, -0.5f) || mock.wheel.capabilityChecks)
        return false;

    DirectInput input(mockForegroundWindow());
    const auto current = [](const void* data) noexcept {
        return !static_cast<const MockController*>(data)->exclusivelyAcquired;
    };
    const auto accepted = input.setWheelForce(wheelId, 0, -0.5f,
        {invalidateDuringAcquire ? +current : nullptr, nullptr, &mock.wheel});
    const bool active = accepted == -0.5f && mock.wheel.force == -5000 &&
        mock.wheel.exclusivelyAcquired && mock.wheel.autocenter == DIPROPAUTOCENTER_OFF &&
        mock.wheel.effects == 1 && input.wheelForceState() == WheelForceState::active;
    const bool rejected = !accepted && !mock.wheel.effects && !mock.wheel.force &&
        !mock.wheel.exclusivelyAcquired && mock.wheel.autocenter == DIPROPAUTOCENTER_ON;
    input.stopWheelForce();
    const bool released = !mock.wheel.force && !mock.wheel.exclusivelyAcquired &&
        mock.wheel.autocenter == DIPROPAUTOCENTER_ON;
    return mock.wheel.exclusiveAcquires == 1 && (invalidateDuringAcquire ? rejected : active) && released;
}

} // namespace

int main()
{
    if (!reportDirectInputInitializationFailure()) {
        std::cerr << "DirectInput initialization failure did not preserve its operation and HRESULT\n";
        return 9;
    }

    if (!forceWithDeferredActuator(false) || !forceWithDeferredActuator(true)) {
        std::cerr << "Force feedback did not recover deferred actuator metadata or safely stop\n";
        return 6;
    }

    MockDriver mock;
    mock.pedals.acquisitionRequired = false;
    driver = &mock;
    ReadEvents events;
    {
        DirectInput input(GetDesktopWindow(), nullptr, &ReadEvents::observe, &events);
        if (!recoverRequiredLayout(mock.wheel, input, events))
            return 1;
    }
    if (!independentDiscovery(mock))
        return 2;
    if (!recoverAfterMetadataError(ProbeFailure::accessDenied)) {
        std::cerr << "A metadata access denial did not recover on the acquired reader\n";
        return 3;
    }
    if (!recoverAfterMetadataError(ProbeFailure::errorAfterAcquireDenial)) {
        std::cerr << "An acquisition refusal followed by a different metadata error prevented recovery\n";
        return 4;
    }
    if (!coldOpenAfterGenericProbeError()) {
        std::cerr << "Cold reopen left the wheel pending after a generic layout error or interrupted the pedals\n";
        return 8;
    }

    driver = &mock;
    mock.wheel.axisProbeFailure = ProbeFailure::accessDenied;
    mock.wheel.buttonPresent = true;
    DirectInput input(GetDesktopWindow());
    const auto inventory = input.devices();
    if (inventory.size() != 1 || !inventory[0].axes[0] || !inventory[0].buttons[0] ||
        !inventory[0].povs[0] || inventory[0].layoutPending.buttons[0]) {
        std::cerr << "Inventory did not retry temporary access failures on an acquired handle\n";
        return 5;
    }

    mock.wheel.setupBlocked = true;
    const auto provisional = input.devices();
    if (provisional.size() != 1 || !provisional[0].layoutPending.axes[0] || provisional[0].axes[0]) {
        std::cerr << "Inventory lost a controller whose handle was temporarily refused\n";
        return 7;
    }
}
