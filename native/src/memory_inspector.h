#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wapi::inspection {

struct MemoryRegionInfo {
    std::uint64_t base = 0;
    std::uint64_t allocationBase = 0;
    std::uint64_t size = 0;
    std::uint32_t state = 0;
    std::uint32_t type = 0;
    std::uint32_t protection = 0;
    std::uint32_t allocationProtection = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    bool guarded = false;
    bool registeredModule = false;
    bool suspicious = false;
    std::string classification;
    std::string moduleName;
    std::string protectionName;
};

struct PeImageInfo {
    bool readable = false;
    bool valid = false;
    bool dll = false;
    bool pe32Plus = false;
    std::uint32_t machine = 0;
    std::uint32_t sectionCount = 0;
    std::uint64_t imageBase = 0;
    std::uint64_t sizeOfImage = 0;
    std::uint64_t entryPoint = 0;
    bool hasImports = false;
    bool hasRelocations = false;
    bool hasTls = false;
    bool hasExceptionDirectory = false;
    std::string machineName;
    std::string reason;
};

struct ThreadStartInfo {
    std::uint32_t threadId = 0;
    std::uint32_t processId = 0;
    std::uint64_t address = 0;
    bool registeredModule = false;
    bool executable = false;
    bool suspicious = false;
    std::string classification;
    std::string moduleName;
};

std::vector<MemoryRegionInfo> memoryRegions(HANDLE process, bool executableOnly = false);
std::vector<MemoryRegionInfo> unbackedExecutableRegions(std::uint32_t pid);
PeImageInfo inspectRemotePe(HANDLE process, std::uint64_t address);
PeImageInfo inspectPeFile(const std::filesystem::path& path);
ThreadStartInfo threadStartAddress(HANDLE thread);

} // namespace wapi::inspection

