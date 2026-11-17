#pragma once

#include "chrome_axis/configuration.hpp"

#include <array>
#include <atomic>

namespace chrome_axis {

// Windows' polled key state includes injected events. Exclude any possible
// output of a key-down attempt until the game restarts, including delayed taps.
class AccessoryKeyPollingGuard {
public:
    bool allowsPolling(unsigned int key) const noexcept
    {
        return isTransmissionHotkey(key) && !injected_[key].load();
    }

    void beforeSend(unsigned int requestedKey, UINT scan, HKL layout) noexcept
    {
        block(requestedKey);
        // Match AccessoryKeys' KEYEVENTF_SCANCODE and KEYEVENTF_EXTENDEDKEY encoding.
        const bool extended = (scan & 0xff00) == 0xe000;
        const UINT sentScan = scan & (extended ? 0xffffu : 0xffu);
        block(MapVirtualKeyExW(sentScan, MAPVK_VSC_TO_VK_EX, layout));
        if (extended)
            return;

        struct KeypadAlias {
            UINT scan;
            UINT digit;
            UINT navigation;
        };
        // The unextended keypad scans may be delivered with either VK when
        // NumLock changes. The separate arrow cluster uses extended scans.
        constexpr std::array aliases{
            KeypadAlias{0x52, VK_NUMPAD0, VK_INSERT},
            KeypadAlias{0x4f, VK_NUMPAD1, VK_END},
            KeypadAlias{0x50, VK_NUMPAD2, VK_DOWN},
            KeypadAlias{0x51, VK_NUMPAD3, VK_NEXT},
            KeypadAlias{0x4b, VK_NUMPAD4, VK_LEFT},
            KeypadAlias{0x4c, VK_NUMPAD5, VK_CLEAR},
            KeypadAlias{0x4d, VK_NUMPAD6, VK_RIGHT},
            KeypadAlias{0x47, VK_NUMPAD7, VK_HOME},
            KeypadAlias{0x48, VK_NUMPAD8, VK_UP},
            KeypadAlias{0x49, VK_NUMPAD9, VK_PRIOR},
        };
        for (const auto& alias : aliases) {
            if (sentScan != alias.scan)
                continue;
            block(alias.digit);
            block(alias.navigation);
            break;
        }
    }

private:
    void block(unsigned int key) noexcept
    {
        if (isTransmissionHotkey(key))
            injected_[key].store(true);
    }

    // Publish the exclusion before calling SendInput. No game-thread lock.
    std::array<std::atomic<bool>, 256> injected_{};
};

} // namespace chrome_axis
