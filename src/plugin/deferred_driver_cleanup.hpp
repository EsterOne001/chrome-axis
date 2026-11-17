#pragma once

#include <Windows.h>

#include <memory>
#include <system_error>
#include <utility>

namespace chrome_axis {

// Driver calls and even their cleanup may block indefinitely. The worker owner
// stays alive on a separate thread with its own module reference until cleanup
// finishes. Releasing it from RED4ext's unload callback never joins a driver.
template <class T>
class DeferredDriverCleanup {
public:
    DeferredDriverCleanup() {
        auto state = std::make_unique<State>();
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(&moduleAnchor_), &state->module))
            throwLastError("Retain the driver cleanup module");
        state->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state->event)
            throwLastError("Create the driver cleanup event");
        thread_ = CreateThread(nullptr, 0, &run, state.get(), 0, nullptr);
        if (!thread_)
            throwLastError("Start the driver cleanup thread");
        state_ = state.release();
    }

    DeferredDriverCleanup(const DeferredDriverCleanup&) = delete;
    DeferredDriverCleanup& operator=(const DeferredDriverCleanup&) = delete;

    ~DeferredDriverCleanup() {
        release();
        CloseHandle(thread_);
    }

    T& workers() noexcept { return state_->workers; }

    void release() noexcept {
        if (const auto state = std::exchange(state_, nullptr))
            SetEvent(state->event);
    }

    bool wait(DWORD timeout) const noexcept {
        return WaitForSingleObject(thread_, timeout) == WAIT_OBJECT_0;
    }

private:
    struct State {
        ~State() {
            if (event)
                CloseHandle(event);
            if (module)
                FreeLibrary(module);
        }

        HMODULE module{};
        HANDLE event{};
        T workers;
    };

    static DWORD WINAPI run(void* opaqueState) noexcept {
        const auto state = static_cast<State*>(opaqueState);
        WaitForSingleObject(state->event, INFINITE);
        const auto module = std::exchange(state->module, nullptr);
        delete state;
        // A plain FreeLibrary followed by returning would race module unload.
        FreeLibraryAndExitThread(module, 0);
    }

    [[noreturn]] static void throwLastError(const char* operation) {
        const auto error = GetLastError();
        throw std::system_error(static_cast<int>(error), std::system_category(), operation);
    }

    static inline const char moduleAnchor_{};
    State* state_{};
    HANDLE thread_{};
};

} // namespace chrome_axis
