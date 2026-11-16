#pragma once

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

#include <cstdint>
#include <filesystem>

namespace RED4ext::vehicle {
struct BaseObject;
}

namespace manual_probe {

void initialize(const std::filesystem::path& pluginDirectory, RED4ext::v1::PluginHandle plugin,
                const RED4ext::v1::Sdk* sdk) noexcept;
void heartbeat(RED4ext::vehicle::BaseObject* vehicle, bool foreground) noexcept;
std::int32_t displayGear(RED4ext::vehicle::BaseObject* vehicle) noexcept;
void shutdown() noexcept;

} // namespace manual_probe