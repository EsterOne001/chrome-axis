#include "transmission_hotkey.hpp"
#include "manual_transmission_input.hpp"
#include "chrome_axis/configuration.hpp"

#include <atomic>
#include <future>
#include <thread>
#include <utility>

namespace chrome_axis {
namespace {

PhysicalModeKey modeKey{defaultTransmissionHotkey};
std::atomic<bool> available{};
HANDLE stopEvent{};
std::thread keyboardThread;

bool gameForeground() noexcept {
    DWORD process{};
    if (const auto window = GetForegroundWindow())
        GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

LRESULT CALLBACK observeKey(int code, WPARAM message, LPARAM data) noexcept {
    if (code == HC_ACTION) {
        const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (key.vkCode == modeKey.key() && !(key.flags & LLKHF_INJECTED)) {
            if (gameForeground())
                modeKey.update(key.vkCode, (key.flags & LLKHF_UP) == 0, false, GetTickCount64());
            else
                modeKey.setContext(false, 0);
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

void observeKeyboard(HMODULE module, HANDLE stop, std::promise<bool> started) noexcept {
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    const auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, observeKey, module, 0);
    if (!hook) {
        started.set_value(false);
        return;
    }

    available.store(true, std::memory_order_release);
    started.set_value(true);
    while (MsgWaitForMultipleObjectsEx(1, &stop, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    available.store(false, std::memory_order_release);
    modeKey.setContext(false, 0);
    UnhookWindowsHookEx(hook);
}

} // namespace

bool startTransmissionHotkey(HMODULE module) {
    if (stopEvent)
        return false;
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent)
        return false;
    try {
        std::promise<bool> started;
        auto startup = started.get_future();
        keyboardThread = std::thread(observeKeyboard, module, stopEvent, std::move(started));
        if (startup.get())
            return true;
    } catch (...) {
        stopTransmissionHotkey();
        return false;
    }
    stopTransmissionHotkey();
    return false;
}

void stopTransmissionHotkey() noexcept {
    if (!stopEvent)
        return;
    SetEvent(stopEvent);
    if (keyboardThread.joinable())
        keyboardThread.join();
    CloseHandle(stopEvent);
    stopEvent = nullptr;
}

void selectTransmissionHotkey(unsigned int key) noexcept {
    modeKey.select(key);
}

unsigned int transmissionHotkeyBinding() noexcept {
    return modeKey.key();
}

void setTransmissionHotkeyContext(bool eligible) noexcept {
    modeKey.setContext(eligible, eligible ? GetTickCount64() : 0);
    if (!eligible)
        return;
    const auto key = modeKey.key();
    if (const auto initial = modeKey.initialState(key);
        initial && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) == 0)
        modeKey.primeReleased(*initial);
}

std::uint64_t transmissionHotkeySampleRevision() noexcept {
    return modeKey.sampleRevision();
}

void sampleTransmissionHotkey(unsigned int key, bool pressed, std::uint64_t observedRevision) noexcept {
    modeKey.sample(key, pressed, GetTickCount64(), observedRevision);
}

std::optional<std::uint64_t> takeTransmissionHotkeyPress(unsigned int key) noexcept {
    const auto press = modeKey.takePress(key, GetTickCount64());
    if (press && !gameForeground()) {
        modeKey.setContext(false, 0);
        return std::nullopt;
    }
    return press;
}

bool transmissionHotkeyAvailable() noexcept { return available.load(std::memory_order_acquire); }

} // namespace chrome_axis
