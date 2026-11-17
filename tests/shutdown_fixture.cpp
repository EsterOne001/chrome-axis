#include "deferred_driver_cleanup.hpp"

#include <array>
#include <memory>
#include <thread>

namespace {

struct DriverCalls {
    ~DriverCalls()
    {
        for (auto& call : calls) {
            if (call.joinable())
                call.join();
        }
        if (cleaned)
            SetEvent(cleaned);
    }

    std::array<std::jthread, 2> calls;
    HANDLE cleaned{};
};

using Cleanup = chrome_axis::DeferredDriverCleanup<DriverCalls>;
std::unique_ptr<Cleanup> cleanup;

} // namespace

extern "C" __declspec(dllexport) BOOL WINAPI StartDriverCalls(HANDLE inputEntered, HANDLE forceEntered,
                                                               HANDLE resumeDriver, HANDLE cleaned)
{
    try {
        cleanup = std::make_unique<Cleanup>();
        auto& workers = cleanup->workers();
        workers.cleaned = cleaned;
        const std::array entered{inputEntered, forceEntered};
        for (std::size_t i = 0; i < entered.size(); ++i) {
            workers.calls[i] = std::jthread([entered = entered[i], resumeDriver] {
                SetEvent(entered);
                // Simulate a synchronous driver that ignores stop requests.
                WaitForSingleObject(resumeDriver, INFINITE);
            });
        }
        return TRUE;
    } catch (...) {
        SetEvent(resumeDriver);
        cleanup.reset();
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI StopDriverCalls(DWORD timeout, BOOL expectPending)
{
    if (!cleanup)
        return FALSE;
    for (auto& call : cleanup->workers().calls)
        call.request_stop();
    cleanup->release();
    const bool finished = cleanup->wait(timeout);
    cleanup.reset();
    return finished != (expectPending != FALSE);
}
