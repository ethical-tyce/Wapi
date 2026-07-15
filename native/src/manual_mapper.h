#pragma once

#include <cstdint>
#include <filesystem>

namespace wapi::injection {

struct ManualMapOptions {
    std::uint32_t timeoutMs = 15000;
    bool validateOnly = false;
};

std::uintptr_t manualMapDll(
    std::uint32_t pid,
    const std::filesystem::path& dllPath,
    const ManualMapOptions& options = {}
);

} // namespace wapi::injection
