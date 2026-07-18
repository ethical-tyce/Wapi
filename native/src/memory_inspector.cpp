#include "memory_inspector.h"

#include <TlHelp32.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace wapi::inspection {
namespace {

struct ModuleRange {
    std::uint64_t base = 0;
    std::uint64_t end = 0;
    std::string name;
};

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

bool allowsRead(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xffu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool allowsWrite(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD base = protection & 0xffu;
    return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool allowsExecute(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD base = protection & 0xffu;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

std::string protectionName(DWORD protection) {
    std::string name;
    switch (protection & 0xffu) {
    case PAGE_NOACCESS: name = "NOACCESS"; break;
    case PAGE_READONLY: name = "R"; break;
    case PAGE_READWRITE: name = "RW"; break;
    case PAGE_WRITECOPY: name = "WC"; break;
    case PAGE_EXECUTE: name = "X"; break;
    case PAGE_EXECUTE_READ: name = "RX"; break;
    case PAGE_EXECUTE_READWRITE: name = "RWX"; break;
    case PAGE_EXECUTE_WRITECOPY: name = "XWC"; break;
    default: name = "UNKNOWN"; break;
    }
    if ((protection & PAGE_GUARD) != 0) name += "|GUARD";
    if ((protection & PAGE_NOCACHE) != 0) name += "|NOCACHE";
    if ((protection & PAGE_WRITECOMBINE) != 0) name += "|WRITECOMBINE";
    return name;
}

std::vector<ModuleRange> modulesForProcess(DWORD pid) {
    std::vector<ModuleRange> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return modules;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            const std::uint64_t base = reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
            if (base <= (std::numeric_limits<std::uint64_t>::max)() - entry.modBaseSize) {
                modules.push_back({base, base + entry.modBaseSize, wideToUtf8(entry.szModule)});
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return modules;
}

const ModuleRange* moduleContaining(const std::vector<ModuleRange>& modules, std::uint64_t address) {
    for (const auto& module : modules) {
        if (address >= module.base && address < module.end) return &module;
    }
    return nullptr;
}

MemoryRegionInfo describeRegion(const MEMORY_BASIC_INFORMATION& raw, const std::vector<ModuleRange>& modules) {
    MemoryRegionInfo region;
    region.base = reinterpret_cast<std::uint64_t>(raw.BaseAddress);
    region.allocationBase = reinterpret_cast<std::uint64_t>(raw.AllocationBase);
    region.size = static_cast<std::uint64_t>(raw.RegionSize);
    region.state = raw.State;
    region.type = raw.Type;
    region.protection = raw.Protect;
    region.allocationProtection = raw.AllocationProtect;
    region.readable = raw.State == MEM_COMMIT && allowsRead(raw.Protect);
    region.writable = raw.State == MEM_COMMIT && allowsWrite(raw.Protect);
    region.executable = raw.State == MEM_COMMIT && allowsExecute(raw.Protect);
    region.guarded = (raw.Protect & PAGE_GUARD) != 0;
    region.protectionName = protectionName(raw.Protect);
    if (const ModuleRange* module = moduleContaining(modules, region.base)) {
        region.registeredModule = true;
        region.moduleName = module->name;
        region.classification = "registered_image";
    } else if (raw.Type == MEM_IMAGE) {
        region.classification = "unregistered_image";
    } else if (raw.Type == MEM_MAPPED) {
        region.classification = "mapped_file";
    } else if (raw.Type == MEM_PRIVATE && region.executable) {
        region.classification = "private_executable";
    } else if (raw.Type == MEM_PRIVATE) {
        region.classification = "private_memory";
    } else {
        region.classification = "unknown";
    }
    region.suspicious = region.executable && !region.registeredModule && raw.Type == MEM_PRIVATE;
    return region;
}

template <typename T>
bool readRemote(HANDLE process, std::uint64_t address, T& value) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), &value, sizeof(value), &read) && read == sizeof(value);
}

std::string machineName(WORD machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386: return "x86";
    case IMAGE_FILE_MACHINE_AMD64: return "x64";
    case IMAGE_FILE_MACHINE_ARM64: return "arm64";
    default: return "unknown";
    }
}

template <typename Reader>
PeImageInfo inspectPe(std::uint64_t base, Reader&& read) {
    PeImageInfo info;
    IMAGE_DOS_HEADER dos{};
    if (!read(base, &dos, sizeof(dos))) {
        info.reason = "DOS header is unreadable";
        return info;
    }
    info.readable = true;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || dos.e_lfanew > 1024 * 1024) {
        info.reason = "invalid DOS header";
        return info;
    }
    const std::uint64_t ntAddress = base + static_cast<std::uint64_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER file{};
    if (!read(ntAddress, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
        !read(ntAddress + sizeof(DWORD), &file, sizeof(file))) {
        info.reason = "invalid or unreadable NT headers";
        return info;
    }
    WORD magic = 0;
    const std::uint64_t optionalAddress = ntAddress + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (!read(optionalAddress, &magic, sizeof(magic))) {
        info.reason = "optional header is unreadable";
        return info;
    }
    info.machine = file.Machine;
    info.machineName = machineName(file.Machine);
    info.sectionCount = file.NumberOfSections;
    info.dll = (file.Characteristics & IMAGE_FILE_DLL) != 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (!read(optionalAddress, &optional, sizeof(optional))) {
            info.reason = "PE32+ optional header is truncated";
            return info;
        }
        info.pe32Plus = true;
        info.imageBase = optional.ImageBase;
        info.sizeOfImage = optional.SizeOfImage;
        info.entryPoint = optional.AddressOfEntryPoint;
        const auto present = [&](std::size_t index) {
            return index < optional.NumberOfRvaAndSizes && optional.DataDirectory[index].VirtualAddress != 0;
        };
        info.hasImports = present(IMAGE_DIRECTORY_ENTRY_IMPORT);
        info.hasRelocations = present(IMAGE_DIRECTORY_ENTRY_BASERELOC);
        info.hasTls = present(IMAGE_DIRECTORY_ENTRY_TLS);
        info.hasExceptionDirectory = present(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        if (!read(optionalAddress, &optional, sizeof(optional))) {
            info.reason = "PE32 optional header is truncated";
            return info;
        }
        info.imageBase = optional.ImageBase;
        info.sizeOfImage = optional.SizeOfImage;
        info.entryPoint = optional.AddressOfEntryPoint;
        const auto present = [&](std::size_t index) {
            return index < optional.NumberOfRvaAndSizes && optional.DataDirectory[index].VirtualAddress != 0;
        };
        info.hasImports = present(IMAGE_DIRECTORY_ENTRY_IMPORT);
        info.hasRelocations = present(IMAGE_DIRECTORY_ENTRY_BASERELOC);
        info.hasTls = present(IMAGE_DIRECTORY_ENTRY_TLS);
        info.hasExceptionDirectory = present(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    } else {
        info.reason = "unsupported optional-header magic";
        return info;
    }
    if (info.sectionCount == 0 || info.sectionCount > 96 || info.sizeOfImage == 0 || info.entryPoint >= info.sizeOfImage) {
        info.reason = "implausible PE layout";
        return info;
    }
    info.valid = true;
    info.reason = "valid PE headers";
    return info;
}

} // namespace

std::vector<MemoryRegionInfo> memoryRegions(HANDLE process, bool executableOnly) {
    if (!process || process == INVALID_HANDLE_VALUE) throw std::runtime_error("invalid process handle");
    const DWORD pid = GetProcessId(process);
    const auto modules = modulesForProcess(pid);
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    std::uint64_t current = reinterpret_cast<std::uint64_t>(system.lpMinimumApplicationAddress);
    const std::uint64_t maximum = reinterpret_cast<std::uint64_t>(system.lpMaximumApplicationAddress);
    std::vector<MemoryRegionInfo> result;
    while (current < maximum) {
        MEMORY_BASIC_INFORMATION raw{};
        if (VirtualQueryEx(process, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(current)), &raw, sizeof(raw)) == 0) break;
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(raw.BaseAddress);
        if (raw.RegionSize == 0 || base > (std::numeric_limits<std::uint64_t>::max)() - raw.RegionSize) break;
        const std::uint64_t next = base + raw.RegionSize;
        if (raw.State == MEM_COMMIT) {
            MemoryRegionInfo region = describeRegion(raw, modules);
            if (!executableOnly || region.executable) result.push_back(std::move(region));
        }
        if (next <= current) break;
        current = next;
    }
    return result;
}

std::vector<MemoryRegionInfo> unbackedExecutableRegions(std::uint32_t pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) throw std::runtime_error("could not open process for memory inspection");
    std::vector<MemoryRegionInfo> all;
    try {
        all = memoryRegions(process, true);
    } catch (...) {
        CloseHandle(process);
        throw;
    }
    CloseHandle(process);
    all.erase(std::remove_if(all.begin(), all.end(), [](const MemoryRegionInfo& region) {
        return !region.suspicious;
    }), all.end());
    return all;
}

PeImageInfo inspectRemotePe(HANDLE process, std::uint64_t address) {
    if (!process || process == INVALID_HANDLE_VALUE || address == 0) return {};
    return inspectPe(address, [process](std::uint64_t at, void* output, std::size_t size) {
        SIZE_T read = 0;
        return ReadProcessMemory(process, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)), output, size, &read) && read == size;
    });
}

PeImageInfo inspectPeFile(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("PE path does not exist");
    const auto length = std::filesystem::file_size(path);
    if (length == 0 || length > 256ull * 1024ull * 1024ull) throw std::runtime_error("PE file size is invalid or excessive");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open PE file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("could not read PE file");
    }
    return inspectPe(0, [&bytes](std::uint64_t at, void* output, std::size_t size) {
        if (at > bytes.size() || size > bytes.size() - static_cast<std::size_t>(at)) return false;
        std::memcpy(output, bytes.data() + static_cast<std::size_t>(at), size);
        return true;
    });
}

ThreadStartInfo threadStartAddress(HANDLE thread) {
    if (!thread || thread == INVALID_HANDLE_VALUE) throw std::runtime_error("invalid thread handle");
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = ntdll ? reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread")) : nullptr;
    if (!query) throw std::runtime_error("NtQueryInformationThread is unavailable");
    void* start = nullptr;
    const LONG status = query(thread, 9u, &start, sizeof(start), nullptr);
    if (status < 0 || !start) throw std::runtime_error("could not query thread start address");
    ThreadStartInfo result;
    result.threadId = GetThreadId(thread);
    result.processId = GetProcessIdOfThread(thread);
    result.address = reinterpret_cast<std::uint64_t>(start);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, result.processId);
    if (!process) return result;
    const auto modules = modulesForProcess(result.processId);
    if (const ModuleRange* module = moduleContaining(modules, result.address)) {
        result.registeredModule = true;
        result.moduleName = module->name;
        result.classification = "registered_image";
    }
    MEMORY_BASIC_INFORMATION raw{};
    if (VirtualQueryEx(process, start, &raw, sizeof(raw)) != 0) {
        MemoryRegionInfo region = describeRegion(raw, modules);
        result.executable = region.executable;
        if (result.classification.empty()) result.classification = region.classification;
        result.suspicious = region.suspicious;
    }
    CloseHandle(process);
    return result;
}

} // namespace wapi::inspection

