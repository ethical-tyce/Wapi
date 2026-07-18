#pragma once

#include <cstdint>
#include <filesystem>
#include <cstddef>

namespace wapi::injection {

struct ManualMapOptions {
    std::uint32_t timeoutMs = 15000;
    bool validateOnly = false;
};

struct ManualMapResult {
    std::uintptr_t baseAddress = 0;
    std::size_t imageSize = 0;
    std::size_t sectionCount = 0;
    std::size_t importModuleCount = 0;
    std::size_t relocationCount = 0;
    std::size_t unwindEntryCount = 0;
    bool validated = false;
    bool entryPointCalled = false;
    bool unwindRegistered = false;
    bool finalProtectionsApplied = false;
};

ManualMapResult manualMapDllDetailed(
    std::uint32_t pid,
    const std::filesystem::path& dllPath,
    const ManualMapOptions& options = {}
);

std::uintptr_t manualMapDll(
    std::uint32_t pid,
    const std::filesystem::path& dllPath,
    const ManualMapOptions& options = {}
);

} // namespace wapi::injection
