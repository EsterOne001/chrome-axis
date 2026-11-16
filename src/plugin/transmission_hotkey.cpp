#include "transmission_hotkey.hpp"
#include "manual_transmission_input.hpp"

#include <atomic>
#include <future>
#include <thread>
#include <utility>

namespace chrome_axis {
namespace {

PhysicalModeKey modeKey;
std::atomic<bool> available{};
HANDLE stopEvent{};
std::thread keyboardThread;

LRESULT CALLBACK observeKey(int code, WPARAM message, LPARAM data) noexcept {
    if (code == HC_ACTION) {
        const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (key.vkCode == VK_F8)
            modeKey.update((key.flags & LLKHF_UP) == 0, (key.flags & LLKHF_INJECTED) != 0);
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

    modeKey.update((GetAsyncKeyState(VK_F8) & 0x8000) != 0, false);
    available.store(true, std::memory_order_release);
    started.set_value(true);
    while (MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    available.store(false, std::memory_order_release);
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

std::optional<bool> transmissionHotkey() noexcept {
    return available.load(std::memory_order_acquire) ? std::optional{modeKey.down()} : std::nullopt;
}

} // namespace chrome_axis
