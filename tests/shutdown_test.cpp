#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bit>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;
using StartCalls = BOOL(WINAPI*)(HANDLE, HANDLE, HANDLE, HANDLE);
using StopCalls = BOOL(WINAPI*)(DWORD, BOOL);

bool check(bool passed, const char* description)
{
    if (!passed)
        std::cerr << description << '\n';
    return passed;
}

bool simulateUnload(const std::filesystem::path& dll, bool blocked, bool exitHost = false)
{
    const Handle inputEntered{CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle};
    const Handle forceEntered{CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle};
    const Handle cleaned{CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle};
    const Handle resumeDriver{CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle};
    Module module{LoadLibraryW(dll.c_str()), FreeLibrary};
    if (!check(inputEntered && forceEntered && cleaned && resumeDriver && module, "Could not initialize fixture"))
        return false;

    const auto start = std::bit_cast<StartCalls>(GetProcAddress(module.get(), "StartDriverCalls"));
    const auto stop = std::bit_cast<StopCalls>(GetProcAddress(module.get(), "StopDriverCalls"));
    if (!check(start && stop && start(inputEntered.get(), forceEntered.get(), resumeDriver.get(), cleaned.get()),
               "Could not start simulated DirectInput readers"))
        return false;

    const HANDLE entered[]{inputEntered.get(), forceEntered.get()};
    if (!check(WaitForMultipleObjects(2, entered, TRUE, 2000) == WAIT_OBJECT_0,
               "The simulated driver calls did not start")) {
        SetEvent(resumeDriver.get());
        stop(2000, FALSE);
        return false;
    }
    if (!blocked)
        SetEvent(resumeDriver.get());
    const bool stopped = stop(blocked ? 30 : 2000, blocked ? TRUE : FALSE) != FALSE;
    module.reset(); // Simulate RED4ext releasing its reference after Main(Unload).
    const auto moduleName = dll.filename();
    if (!blocked)
        return check(stopped && WaitForSingleObject(cleaned.get(), 0) == WAIT_OBJECT_0 &&
                     !GetModuleHandleW(moduleName.c_str()), "A healthy driver did not shut down and release the DLL");

    const bool retained = check(stopped && WaitForSingleObject(cleaned.get(), 0) == WAIT_TIMEOUT &&
                               GetModuleHandleW(moduleName.c_str()), "The live driver calls lost ownership of the DLL");
    if (exitHost)
        ExitProcess(retained ? 0 : 1); // Leave both calls blocked as the host exits.

    SetEvent(resumeDriver.get());
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds{2};
    while (GetModuleHandleW(moduleName.c_str()) && Clock::now() < deadline)
        Sleep(5);
    return check(retained && WaitForSingleObject(cleaned.get(), 0) == WAIT_OBJECT_0 &&
                 !GetModuleHandleW(moduleName.c_str()), "The DLL was not released after both driver calls returned");
}

bool testExitWithBlockedCalls(const std::filesystem::path& self, const std::filesystem::path& dll)
{
    auto command = L"\"" + self.native() + L"\" \"" + dll.native() + L"\" --exit-with-blocked-calls";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!check(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &startup, &process), "Could not start simulated game process"))
        return false;
    const Handle child{process.hProcess, CloseHandle};
    const Handle mainThread{process.hThread, CloseHandle};
    if (WaitForSingleObject(child.get(), 5000) != WAIT_OBJECT_0) {
        TerminateProcess(child.get(), 1); // This fixture owns the simulated process.
        WaitForSingleObject(child.get(), 1000);
        return check(false, "The simulated game could not exit with blocked driver calls");
    }
    DWORD code{};
    return check(GetExitCodeProcess(child.get(), &code) && code == 0,
                 "The blocked-driver exit fixture failed");
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc == 3 && std::wstring_view(argv[2]) == L"--exit-with-blocked-calls")
        return simulateUnload(argv[1], true, true) ? 0 : 1;
    if (argc != 2)
        return 1;
    return simulateUnload(argv[1], false) && simulateUnload(argv[1], true) &&
        testExitWithBlockedCalls(argv[0], argv[1]) ? 0 : 1;
}
