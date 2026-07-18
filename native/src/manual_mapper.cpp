#include "manual_mapper.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace wapi::injection {
namespace {

constexpr std::size_t kMaxFileSize = 256ull * 1024 * 1024;
constexpr std::size_t kMaxImageSize = 512ull * 1024 * 1024;
constexpr std::size_t kMaxSections = 96;
constexpr std::size_t kMaxImports = 16384;
constexpr std::size_t kMaxExceptionEntries = 1u << 20;
constexpr std::uint32_t kCleanupTimeoutMs = 2000;


// RUNTIME_FUNCTION follows the host architecture selected by the Windows SDK.
// The mapper always parses an x64 PE image, so keep its on-disk layout explicit.
struct X64RuntimeFunctionEntry {
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
};
static_assert(sizeof(X64RuntimeFunctionEntry) == 12);
[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("Manual map: " + message);
}

[[noreturn]] void failWin32(const std::string& message) {
    fail(message + " (Win32 error " + std::to_string(GetLastError()) + ")");
}

bool rangeInside(std::size_t offset, std::size_t size, std::size_t total) {
    return offset <= total && size <= total - offset;
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_;
};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) fail("could not open DLL: " + path.string());
    const std::streamoff length = input.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > kMaxFileSize) {
        fail("DLL file size is invalid or exceeds 256 MiB");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), length)) {
        fail("could not read the complete DLL file");
    }
    return bytes;
}

struct ParsedImage {
    std::vector<std::uint8_t> bytes;
    IMAGE_OPTIONAL_HEADER64 optional{};
    std::vector<IMAGE_SECTION_HEADER> sections;

    IMAGE_DATA_DIRECTORY directory(std::size_t index) const {
        if (index >= optional.NumberOfRvaAndSizes || index >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return {};
        return optional.DataDirectory[index];
    }

    template <typename T>
    T* at(std::size_t rva, std::size_t count = 1) {
        if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) fail("PE range overflow");
        const std::size_t size = sizeof(T) * count;
        if (!rangeInside(rva, size, bytes.size())) fail("PE RVA points outside the mapped image");
        return reinterpret_cast<T*>(bytes.data() + rva);
    }

    template <typename T>
    const T* at(std::size_t rva, std::size_t count = 1) const {
        if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) fail("PE range overflow");
        const std::size_t size = sizeof(T) * count;
        if (!rangeInside(rva, size, bytes.size())) fail("PE RVA points outside the mapped image");
        return reinterpret_cast<const T*>(bytes.data() + rva);
    }

    std::string stringAt(std::size_t rva, std::size_t maxLength = 1024) const {
        if (rva >= bytes.size()) fail("PE string RVA points outside the mapped image");
        const std::size_t available = (std::min)(maxLength, bytes.size() - rva);
        const char* value = reinterpret_cast<const char*>(bytes.data() + rva);
        for (std::size_t index = 0; index < available; ++index) {
            if (value[index] == '\0') return std::string(value, index);
        }
        fail("PE string is not null terminated within its allowed range");
    }
};

ParsedImage parseImage(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> file = readFile(path);
    if (!rangeInside(0, sizeof(IMAGE_DOS_HEADER), file.size())) fail("truncated DOS header");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))) {
        fail("invalid DOS header");
    }

    const std::size_t ntOffset = static_cast<std::size_t>(dos->e_lfanew);
    const std::size_t fileHeaderOffset = ntOffset + sizeof(DWORD);
    if (!rangeInside(ntOffset, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER), file.size())) fail("truncated NT headers");
    if (*reinterpret_cast<const DWORD*>(file.data() + ntOffset) != IMAGE_NT_SIGNATURE) fail("invalid NT signature");
    const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(file.data() + fileHeaderOffset);
    if (fileHeader->Machine != IMAGE_FILE_MACHINE_AMD64) fail("v1 manual mapping accepts x64 DLLs only");
    if ((fileHeader->Characteristics & IMAGE_FILE_DLL) == 0) fail("PE image is not marked as a DLL");
    if (fileHeader->NumberOfSections == 0 || fileHeader->NumberOfSections > kMaxSections) fail("invalid PE section count");

    const std::size_t optionalOffset = fileHeaderOffset + sizeof(IMAGE_FILE_HEADER);
    if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !rangeInside(optionalOffset, fileHeader->SizeOfOptionalHeader, file.size())) {
        fail("truncated or unsupported optional header");
    }
    const auto* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(file.data() + optionalOffset);
    if (optional->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) fail("DLL is not PE32+");
    if (optional->SizeOfImage == 0 || optional->SizeOfImage > kMaxImageSize) fail("invalid or excessive image size");
    if (optional->SizeOfHeaders == 0 || optional->SizeOfHeaders > optional->SizeOfImage || optional->SizeOfHeaders > file.size()) {
        fail("invalid PE header size");
    }
    if (optional->SectionAlignment < 0x1000) fail("section alignment below one page is not supported");
    if (optional->AddressOfEntryPoint >= optional->SizeOfImage) fail("entry point is outside the image");
    if ((optional->DllCharacteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF) != 0) {
        fail("CFG-instrumented DLLs are not supported by the bounded mapper");
    }

    const std::size_t sectionOffset = optionalOffset + fileHeader->SizeOfOptionalHeader;
    const std::size_t sectionBytes = static_cast<std::size_t>(fileHeader->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (!rangeInside(sectionOffset, sectionBytes, file.size())) fail("truncated section table");

    ParsedImage image;
    image.optional = *optional;
    image.bytes.resize(optional->SizeOfImage);
    std::memcpy(image.bytes.data(), file.data(), optional->SizeOfHeaders);
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(file.data() + sectionOffset);
    image.sections.assign(sections, sections + fileHeader->NumberOfSections);

    std::vector<std::pair<std::size_t, std::size_t>> occupied;
    occupied.emplace_back(0, optional->SizeOfHeaders);
    for (const auto& section : image.sections) {
        const std::size_t memorySize = (std::max)(
            static_cast<std::size_t>(section.Misc.VirtualSize),
            static_cast<std::size_t>(section.SizeOfRawData)
        );
        if (memorySize == 0) continue;
        if (section.VirtualAddress % optional->SectionAlignment != 0 ||
            !rangeInside(section.VirtualAddress, memorySize, image.bytes.size())) {
            fail("section virtual range is invalid");
        }
        for (const auto& [start, end] : occupied) {
            const std::size_t sectionEnd = static_cast<std::size_t>(section.VirtualAddress) + memorySize;
            if (section.VirtualAddress < end && start < sectionEnd) fail("overlapping PE sections are not supported");
        }
        occupied.emplace_back(section.VirtualAddress, static_cast<std::size_t>(section.VirtualAddress) + memorySize);
        if (section.SizeOfRawData != 0) {
            if (!rangeInside(section.PointerToRawData, section.SizeOfRawData, file.size())) fail("section raw range is invalid");
            std::memcpy(image.bytes.data() + section.VirtualAddress, file.data() + section.PointerToRawData, section.SizeOfRawData);
        }
        const bool executable = (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        const bool writable = (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        if (executable && writable) fail("writable-executable sections are rejected");
    }

    for (std::size_t index = 0; index < (std::min<std::size_t>)(optional->NumberOfRvaAndSizes, IMAGE_NUMBEROF_DIRECTORY_ENTRIES); ++index) {
        if (index == IMAGE_DIRECTORY_ENTRY_SECURITY) continue;
        const auto directory = optional->DataDirectory[index];
        if (directory.VirtualAddress == 0 && directory.Size == 0) continue;
        if (directory.VirtualAddress == 0 || directory.Size == 0 ||
            !rangeInside(directory.VirtualAddress, directory.Size, image.bytes.size())) {
            fail("PE data directory is outside the image");
        }
    }
    if (image.directory(IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR).VirtualAddress != 0) {
        fail("managed or mixed-mode DLLs need a separate runtime bootstrap");
    }
    if (image.directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT).VirtualAddress != 0) {
        fail("delay imports are not supported by the bounded mapper");
    }
    return image;
}

std::wstring lowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

USHORT imageMachineFromFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    if (!input.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        fail("target executable has an invalid DOS header");
    }
    input.seekg(static_cast<std::streamoff>(dos.e_lfanew), std::ios::beg);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    if (!input.read(reinterpret_cast<char*>(&signature), sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
        !input.read(reinterpret_cast<char*>(&header), sizeof(header))) {
        fail("target executable has invalid NT headers");
    }
    return header.Machine;
}

void validateTarget(std::uint32_t pid, HANDLE process) {
#if !defined(_M_X64)
    (void)pid;
    (void)process;
    fail("manual mapping requires the x64 Wapi build");
#else
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) fail("invalid, system, or self target PID");
    std::vector<wchar_t> pathBuffer(32768);
    DWORD pathLength = static_cast<DWORD>(pathBuffer.size());
    if (!QueryFullProcessImageNameW(process, 0, pathBuffer.data(), &pathLength)) failWin32("could not identify target process");
    const std::filesystem::path targetPath(std::wstring(pathBuffer.data(), pathLength));
    const std::wstring processName = lowerWide(targetPath.filename().wstring());
    static const std::unordered_set<std::wstring> blockedProcesses = {
        L"csrss.exe", L"lsass.exe", L"services.exe", L"smss.exe", L"wininit.exe", L"winlogon.exe"
    };
    if (blockedProcesses.count(processName) != 0) fail("manual mapping into critical Windows processes is blocked");
    using IsProcessCriticalFn = BOOL(WINAPI*)(HANDLE, PBOOL);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) fail("kernel32.dll is unavailable in the Wapi process");
    const auto isProcessCritical = reinterpret_cast<IsProcessCriticalFn>(
        GetProcAddress(kernel32, "IsProcessCritical")
    );
    if (!isProcessCritical) fail("IsProcessCritical is unavailable on this Windows version");
    BOOL critical = FALSE;
    if (!isProcessCritical(process, &critical)) {
        failWin32("could not verify the target critical-process state");
    }
    if (critical) fail("manual mapping into a critical Windows process is blocked");

    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(kernel32, "IsWow64Process2")
    );
    if (!isWow64Process2) fail("IsWow64Process2 is unavailable on this Windows version");
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!isWow64Process2(process, &processMachine, &nativeMachine)) failWin32("could not determine target architecture");
    if (processMachine != IMAGE_FILE_MACHINE_UNKNOWN && processMachine != IMAGE_FILE_MACHINE_AMD64) {
        fail("Wapi, target process, and DLL must all be x64");
    }
    if (nativeMachine != IMAGE_FILE_MACHINE_AMD64 && nativeMachine != IMAGE_FILE_MACHINE_ARM64) {
        fail("unsupported native Windows architecture");
    }
    if (imageMachineFromFile(targetPath) != IMAGE_FILE_MACHINE_AMD64) {
        fail("target executable is not x64");
    }

    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
    if (!GetProcessMitigationPolicy(process, ProcessControlFlowGuardPolicy, &cfg, sizeof(cfg))) {
        failWin32("could not query target CFG policy");
    }
    if (cfg.EnableControlFlowGuard) fail("CFG-enabled target processes are not supported by the bounded mapper");

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamicCode{};
    if (!GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dynamicCode, sizeof(dynamicCode))) {
        failWin32("could not query target dynamic-code policy");
    }
    if (dynamicCode.ProhibitDynamicCode) {
        fail("target dynamic-code policy prohibits manual mapping");
    }

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
    if (!GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature, sizeof(signature))) {
        failWin32("could not query target signature policy");
    }
    if (signature.MicrosoftSignedOnly || signature.StoreSignedOnly || signature.MitigationOptIn) {
        fail("target signature policy prohibits bypassing the Windows image loader");
    }

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
    if (!GetProcessMitigationPolicy(process, ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad))) {
        failWin32("could not query target image-load policy");
    }
    if (imageLoad.NoRemoteImages || imageLoad.NoLowMandatoryLabelImages || imageLoad.PreferSystem32Images) {
        fail("target image-load policy is incompatible with manual mapping");
    }
#endif
}

std::uintptr_t remoteModuleBase(std::uint32_t pid, const std::wstring& moduleName) {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) failWin32("could not enumerate target modules");
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.get(), &module)) {
        do {
            if (_wcsicmp(module.szModule, moduleName.c_str()) == 0) {
                return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
            }
        } while (Module32NextW(snapshot.get(), &module));
    }
    fail("required target module is not loaded: " + std::filesystem::path(moduleName).string());
}

std::uintptr_t remoteAddressForLocalFunction(std::uint32_t pid, FARPROC localFunction) {
    if (!localFunction) fail("required local function is unavailable");
    HMODULE containingModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localFunction),
            &containingModule)) {
        failWin32("could not identify the module containing a loader function");
    }
    std::vector<wchar_t> modulePath(32768);
    const DWORD length = GetModuleFileNameW(containingModule, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length == modulePath.size()) failWin32("could not identify loader module path");
    const std::wstring moduleName = std::filesystem::path(std::wstring(modulePath.data(), length)).filename().wstring();
    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(localFunction) - reinterpret_cast<std::uintptr_t>(containingModule);
    return remoteModuleBase(pid, moduleName) + offset;
}

class Deadline {
public:
    explicit Deadline(std::uint32_t timeoutMs)
        : end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs == 0 ? 1u : timeoutMs)) {}

    DWORD remainingMs() const {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return 0;
        return static_cast<DWORD>((std::min<long long>)(remaining, (std::numeric_limits<DWORD>::max)()));
    }
private:
    std::chrono::steady_clock::time_point end_;
};

class RemoteAllocation {
public:
    RemoteAllocation(HANDLE process, void* address, bool& uncertain)
        : process_(process), address_(address), uncertain_(uncertain) {}
    ~RemoteAllocation() {
        if (owned_ && address_ && !uncertain_) VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
    }
    RemoteAllocation(const RemoteAllocation&) = delete;
    RemoteAllocation& operator=(const RemoteAllocation&) = delete;
    RemoteAllocation(RemoteAllocation&& other) noexcept
        : process_(other.process_), address_(other.address_), uncertain_(other.uncertain_), owned_(other.owned_) {
        other.owned_ = false;
        other.address_ = nullptr;
    }
    RemoteAllocation& operator=(RemoteAllocation&&) = delete;
    void* get() const { return address_; }
    std::uintptr_t value() const { return reinterpret_cast<std::uintptr_t>(address_); }
    void release() { owned_ = false; }
private:
    HANDLE process_;
    void* address_;
    bool& uncertain_;
    bool owned_ = true;
};

void writeRemote(HANDLE process, void* destination, const void* source, std::size_t size, const char* context) {
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, destination, source, size, &written) || written != size) failWin32(context);
}

struct RemoteCallContext {
    std::uintptr_t function = 0;
    std::uintptr_t arg1 = 0;
    std::uintptr_t arg2 = 0;
    std::uintptr_t arg3 = 0;
    std::uintptr_t result = 0;
    std::uint32_t completed = 0;
    std::uint32_t reserved = 0;
};

class RemoteCaller {
public:
    RemoteCaller(HANDLE process, Deadline& deadline, bool& uncertain)
        : process_(process), deadline_(deadline), uncertain_(uncertain) {
#if !defined(_M_X64)
        fail("remote call thunk is implemented for x64 only");
#else
        static constexpr std::array<std::uint8_t, 44> thunk = {
            0x53,
            0x48, 0x83, 0xEC, 0x20,
            0x48, 0x89, 0xCB,
            0x48, 0x8B, 0x03,
            0x48, 0x8B, 0x4B, 0x08,
            0x48, 0x8B, 0x53, 0x10,
            0x4C, 0x8B, 0x43, 0x18,
            0xFF, 0xD0,
            0x48, 0x89, 0x43, 0x20,
            0xC7, 0x43, 0x28, 0x01, 0x00, 0x00, 0x00,
            0x31, 0xC0,
            0x48, 0x83, 0xC4, 0x20,
            0x5B,
            0xC3
        };
        static_assert(offsetof(RemoteCallContext, result) == 0x20);
        static_assert(offsetof(RemoteCallContext, completed) == 0x28);
        try {
            stub_ = VirtualAllocEx(process_, nullptr, thunk.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            context_ = VirtualAllocEx(process_, nullptr, sizeof(RemoteCallContext), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!stub_ || !context_) failWin32("could not allocate the remote call thunk");
            writeRemote(process_, stub_, thunk.data(), thunk.size(), "could not write the remote call thunk");
            DWORD oldProtection = 0;
            if (!VirtualProtectEx(process_, stub_, thunk.size(), PAGE_EXECUTE_READ, &oldProtection)) {
                failWin32("could not protect the remote call thunk");
            }
            if (!FlushInstructionCache(process_, stub_, thunk.size())) {
                failWin32("could not flush the remote call thunk");
            }
        } catch (...) {
            if (context_) VirtualFreeEx(process_, context_, 0, MEM_RELEASE);
            if (stub_) VirtualFreeEx(process_, stub_, 0, MEM_RELEASE);
            context_ = nullptr;
            stub_ = nullptr;
            throw;
        }
#endif
    }

    ~RemoteCaller() {
        if (!uncertain_) {
            if (context_) VirtualFreeEx(process_, context_, 0, MEM_RELEASE);
            if (stub_) VirtualFreeEx(process_, stub_, 0, MEM_RELEASE);
        }
    }

    std::uintptr_t invoke(std::uintptr_t function, std::uintptr_t arg1 = 0, std::uintptr_t arg2 = 0, std::uintptr_t arg3 = 0) {
        return invokeUntil(deadline_, function, arg1, arg2, arg3);
    }

    std::uintptr_t invokeForCleanup(
        std::uintptr_t function,
        std::uintptr_t arg1 = 0,
        std::uintptr_t arg2 = 0,
        std::uintptr_t arg3 = 0
    ) {
        Deadline cleanupDeadline(kCleanupTimeoutMs);
        return invokeUntil(cleanupDeadline, function, arg1, arg2, arg3);
    }

    std::uintptr_t invokeBefore(
        Deadline& deadline,
        std::uintptr_t function,
        std::uintptr_t arg1 = 0,
        std::uintptr_t arg2 = 0,
        std::uintptr_t arg3 = 0
    ) {
        return invokeUntil(deadline, function, arg1, arg2, arg3);
    }
private:
    std::uintptr_t invokeUntil(
        Deadline& deadline,
        std::uintptr_t function,
        std::uintptr_t arg1,
        std::uintptr_t arg2,
        std::uintptr_t arg3
    ) {
        if (!function) fail("attempted to call a null remote function");
        const DWORD waitMs = deadline.remainingMs();
        if (waitMs == 0) fail("operation timed out before a remote call could start");
        RemoteCallContext context{};
        context.function = function;
        context.arg1 = arg1;
        context.arg2 = arg2;
        context.arg3 = arg3;
        writeRemote(process_, context_, &context, sizeof(context), "could not write remote call context");

        ScopedHandle thread(CreateRemoteThread(
            process_, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(stub_),
            context_, 0, nullptr
        ));
        if (!thread) failWin32("could not create the bounded remote call thread");
        const DWORD waitResult = WaitForSingleObject(thread.get(), waitMs);
        if (waitResult != WAIT_OBJECT_0) {
            uncertain_ = true;
            if (waitResult == WAIT_TIMEOUT) fail("remote call timed out; target state is uncertain and allocations were retained");
            failWin32("remote call wait failed; target state is uncertain and allocations were retained");
        }

        SIZE_T read = 0;
        if (!ReadProcessMemory(process_, context_, &context, sizeof(context), &read) || read != sizeof(context)) {
            uncertain_ = true;
            failWin32("could not confirm remote call result; target state is uncertain and allocations were retained");
        }
        if (context.completed != 1) {
            uncertain_ = true;
            fail("remote call returned without a completion marker; target state is uncertain and allocations were retained");
        }
        return context.result;
    }
    HANDLE process_;
    Deadline& deadline_;
    bool& uncertain_;
    void* stub_ = nullptr;
    void* context_ = nullptr;
};

RemoteAllocation allocateRemoteBytes(HANDLE process, const void* bytes, std::size_t size, bool& uncertain) {
    void* remote = VirtualAllocEx(process, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote) failWin32("could not allocate remote helper data");
    RemoteAllocation allocation(process, remote, uncertain);
    writeRemote(process, remote, bytes, size, "could not write remote helper data");
    return allocation;
}

std::uintptr_t invokeWithString(
    RemoteCaller& caller,
    HANDLE process,
    std::uintptr_t function,
    const std::string& value,
    bool& uncertain,
    std::uintptr_t arg1 = 0
) {
    RemoteAllocation remoteString = allocateRemoteBytes(process, value.c_str(), value.size() + 1, uncertain);
    if (arg1 == 0) return caller.invoke(function, remoteString.value());
    return caller.invoke(function, arg1, remoteString.value());
}

class DependencyRollback {
public:
    DependencyRollback(RemoteCaller& caller, std::uintptr_t freeLibrary, bool& uncertain)
        : caller_(caller), freeLibrary_(freeLibrary), uncertain_(uncertain) {}
    ~DependencyRollback() {
        if (released_ || uncertain_) return;
        Deadline cleanupDeadline(kCleanupTimeoutMs);
        for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) {
            try {
                if (caller_.invokeBefore(cleanupDeadline, freeLibrary_, *it) == 0) {
                    uncertain_ = true;
                    break;
                }
            } catch (...) {
                uncertain_ = true;
                break;
            }
        }
    }
    void add(std::uintptr_t module) { handles_.push_back(module); }
    void release() { released_ = true; }
private:
    RemoteCaller& caller_;
    std::uintptr_t freeLibrary_;
    bool& uncertain_;
    std::vector<std::uintptr_t> handles_;
    bool released_ = false;
};

class FunctionTableRollback {
public:
    FunctionTableRollback(RemoteCaller& caller, std::uintptr_t deleteFunctionTable, std::uintptr_t table, bool& uncertain)
        : caller_(caller), deleteFunctionTable_(deleteFunctionTable), table_(table), uncertain_(uncertain) {}
    ~FunctionTableRollback() {
        if (released_ || uncertain_ || table_ == 0) return;
        try {
            if (caller_.invokeForCleanup(deleteFunctionTable_, table_) == 0) uncertain_ = true;
        } catch (...) {
            uncertain_ = true;
        }
    }
    void release() { released_ = true; }
private:
    RemoteCaller& caller_;
    std::uintptr_t deleteFunctionTable_;
    std::uintptr_t table_;
    bool& uncertain_;
    bool released_ = false;
};

FARPROC requiredProc(const wchar_t* moduleName, const char* functionName) {
    HMODULE module = GetModuleHandleW(moduleName);
    FARPROC proc = module ? GetProcAddress(module, functionName) : nullptr;
    if (!proc) fail(std::string("required loader API is unavailable: ") + functionName);
    return proc;
}

std::size_t processRelocations(ParsedImage& image, std::uint64_t delta, bool applyChanges) {
    const auto directory = image.directory(IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (directory.VirtualAddress == 0) {
        if (directory.Size != 0) fail("relocation directory has a size without an RVA");
        if (delta != 0) fail("preferred image base is unavailable and the DLL has no relocation table");
        return 0;
    }
    if (directory.Size < sizeof(IMAGE_BASE_RELOCATION)) fail("truncated relocation directory");

    std::size_t consumed = 0;
    std::size_t relocationCount = 0;
    while (consumed < directory.Size) {
        if (directory.Size - consumed < sizeof(IMAGE_BASE_RELOCATION)) fail("truncated relocation block");
        auto* block = image.at<IMAGE_BASE_RELOCATION>(directory.VirtualAddress + consumed);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > directory.Size - consumed) {
            fail("invalid relocation block size");
        }
        const std::size_t payloadBytes = block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION);
        if (payloadBytes % sizeof(WORD) != 0) fail("misaligned relocation entries");
        const std::size_t entryCount = payloadBytes / sizeof(WORD);
        auto* entries = image.at<WORD>(directory.VirtualAddress + consumed + sizeof(IMAGE_BASE_RELOCATION), entryCount);
        for (std::size_t index = 0; index < entryCount; ++index) {
            const WORD type = entries[index] >> 12;
            const WORD offset = entries[index] & 0x0fff;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64) fail("unsupported x64 relocation type " + std::to_string(type));
            ++relocationCount;
            const std::size_t targetRva = static_cast<std::size_t>(block->VirtualAddress) + offset;
            auto* target = image.at<ULONGLONG>(targetRva);
            if (applyChanges) *target += delta;
        }
        consumed += block->SizeOfBlock;
    }
    if (consumed != directory.Size) fail("relocation directory has trailing bytes");
    return relocationCount;
}

std::size_t validateRelocations(ParsedImage& image) {
    return processRelocations(image, 0, false);
}

void applyRelocations(ParsedImage& image, std::uintptr_t remoteBase) {
    const std::uint64_t delta =
        static_cast<std::uint64_t>(remoteBase) - static_cast<std::uint64_t>(image.optional.ImageBase);
    processRelocations(image, delta, true);
}

void validateTls(const ParsedImage& image, std::uintptr_t remoteBase) {
    const auto directory = image.directory(IMAGE_DIRECTORY_ENTRY_TLS);
    if (directory.VirtualAddress == 0) return;
    if (directory.Size < sizeof(IMAGE_TLS_DIRECTORY64)) fail("truncated TLS directory");
    const auto* tls = image.at<IMAGE_TLS_DIRECTORY64>(directory.VirtualAddress);
    if (tls->EndAddressOfRawData < tls->StartAddressOfRawData) fail("invalid TLS template range");
    if (tls->EndAddressOfRawData > tls->StartAddressOfRawData || tls->SizeOfZeroFill != 0) {
        fail("static TLS or thread_local storage is not supported by the bounded mapper");
    }
    if (tls->AddressOfCallBacks != 0) {
        if (tls->AddressOfCallBacks < remoteBase) fail("TLS callback table points below the image");
        const std::size_t callbacksRva = static_cast<std::size_t>(tls->AddressOfCallBacks - remoteBase);
        const auto* firstCallback = image.at<ULONGLONG>(callbacksRva);
        if (*firstCallback != 0) {
            fail("TLS callbacks are not supported because initialization must stay on one loader thread");
        }
    }
}

std::size_t validateImports(const ParsedImage& image) {
    const auto directory = image.directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (directory.VirtualAddress == 0) {
        if (directory.Size != 0) fail("import directory has a size without an RVA");
        return 0;
    }
    const std::size_t descriptorLimit = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if (descriptorLimit == 0 || descriptorLimit > kMaxImports) fail("invalid import directory size");
    std::size_t totalImports = 0;
    std::size_t moduleCount = 0;
    bool foundTerminator = false;

    for (std::size_t descriptorIndex = 0; descriptorIndex < descriptorLimit; ++descriptorIndex) {
        const auto* descriptor = image.at<IMAGE_IMPORT_DESCRIPTOR>(
            directory.VirtualAddress + descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR)
        );
        if (descriptor->OriginalFirstThunk == 0 && descriptor->TimeDateStamp == 0 &&
            descriptor->ForwarderChain == 0 && descriptor->Name == 0 && descriptor->FirstThunk == 0) {
            foundTerminator = true;
            break;
        }
        if (descriptor->Name == 0 || descriptor->FirstThunk == 0) fail("invalid import descriptor");
        ++moduleCount;
        if (image.stringAt(descriptor->Name, MAX_PATH).empty()) fail("empty imported module name");
        const DWORD lookupRva = descriptor->OriginalFirstThunk != 0
            ? descriptor->OriginalFirstThunk
            : descriptor->FirstThunk;
        bool thunkTerminated = false;
        for (std::size_t thunkIndex = 0; totalImports < kMaxImports; ++thunkIndex) {
            const std::size_t thunkOffset = thunkIndex * sizeof(IMAGE_THUNK_DATA64);
            const auto* lookup = image.at<IMAGE_THUNK_DATA64>(
                static_cast<std::size_t>(lookupRva) + thunkOffset
            );
            image.at<IMAGE_THUNK_DATA64>(
                static_cast<std::size_t>(descriptor->FirstThunk) + thunkOffset
            );
            const ULONGLONG lookupValue = lookup->u1.AddressOfData;
            if (lookupValue == 0) {
                thunkTerminated = true;
                break;
            }
            ++totalImports;
            if (!IMAGE_SNAP_BY_ORDINAL64(lookupValue)) {
                if (lookupValue > (std::numeric_limits<DWORD>::max)()) fail("import-by-name RVA is invalid");
                const std::size_t importRva = static_cast<std::size_t>(lookupValue);
                image.at<WORD>(importRva);
                if (image.stringAt(importRva + sizeof(WORD), 1024).empty()) {
                    fail("empty imported function name");
                }
            }
        }
        if (!thunkTerminated) fail("import table is unterminated or exceeds the import limit");
    }
    if (!foundTerminator) fail("import descriptor table is not terminated");
    return moduleCount;
}

void validateExceptionDirectory(const ParsedImage& image) {
    const auto directory = image.directory(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (directory.VirtualAddress == 0) {
        if (directory.Size != 0) fail("exception directory has a size without an RVA");
        return;
    }
    if (directory.Size == 0 || directory.Size % sizeof(X64RuntimeFunctionEntry) != 0) {
        fail("invalid x64 exception directory size");
    }
    const std::size_t functionCount = directory.Size / sizeof(X64RuntimeFunctionEntry);
    if (functionCount == 0 || functionCount > kMaxExceptionEntries) {
        fail("x64 exception directory exceeds the entry limit");
    }
    const auto* functions = image.at<X64RuntimeFunctionEntry>(directory.VirtualAddress, functionCount);
    DWORD previousEnd = 0;
    for (std::size_t index = 0; index < functionCount; ++index) {
        const auto& function = functions[index];
        if (function.BeginAddress >= function.EndAddress || function.EndAddress > image.optional.SizeOfImage) {
            fail("x64 exception entry points outside the image");
        }
        if (index != 0 && function.BeginAddress < previousEnd) {
            fail("x64 exception entries are unsorted or overlapping");
        }
        if (function.UnwindData == 0 || (function.UnwindData & 0x3u) != 0 ||
            function.UnwindData >= image.optional.SizeOfImage) {
            fail("x64 exception entry has invalid unwind data");
        }
        image.at<std::uint8_t>(function.UnwindData);
        previousEnd = function.EndAddress;
    }
}

void resolveImports(
    ParsedImage& image,
    HANDLE process,
    RemoteCaller& caller,
    DependencyRollback& dependencies,
    std::uintptr_t loadLibrary,
    std::uintptr_t getProcAddress,
    bool& uncertain
) {
    validateImports(image);
    const auto directory = image.directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (directory.VirtualAddress == 0) return;
    const std::size_t descriptorLimit = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if (descriptorLimit == 0) fail("truncated import directory");
    std::size_t totalImports = 0;
    bool foundTerminator = false;

    for (std::size_t descriptorIndex = 0; descriptorIndex < descriptorLimit; ++descriptorIndex) {
        auto* descriptor = image.at<IMAGE_IMPORT_DESCRIPTOR>(
            directory.VirtualAddress + descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR)
        );
        if (descriptor->Name == 0 && descriptor->FirstThunk == 0 && descriptor->OriginalFirstThunk == 0) {
            foundTerminator = true;
            break;
        }
        if (descriptor->Name == 0 || descriptor->FirstThunk == 0) fail("invalid import descriptor");
        const std::string moduleName = image.stringAt(descriptor->Name, MAX_PATH);
        if (moduleName.empty()) fail("empty imported module name");
        const std::uintptr_t remoteModule = invokeWithString(caller, process, loadLibrary, moduleName, uncertain);
        if (remoteModule == 0) fail("target could not load dependency: " + moduleName);
        dependencies.add(remoteModule);

        const DWORD lookupRva = descriptor->OriginalFirstThunk != 0
            ? descriptor->OriginalFirstThunk
            : descriptor->FirstThunk;
        bool thunkTerminated = false;
        for (std::size_t thunkIndex = 0; totalImports < kMaxImports; ++thunkIndex, ++totalImports) {
            const std::size_t thunkOffset = thunkIndex * sizeof(IMAGE_THUNK_DATA64);
            auto* lookup = image.at<IMAGE_THUNK_DATA64>(static_cast<std::size_t>(lookupRva) + thunkOffset);
            auto* destination = image.at<IMAGE_THUNK_DATA64>(static_cast<std::size_t>(descriptor->FirstThunk) + thunkOffset);
            const ULONGLONG lookupValue = lookup->u1.AddressOfData;
            if (lookupValue == 0) {
                thunkTerminated = true;
                break;
            }

            std::uintptr_t remoteFunction = 0;
            if (IMAGE_SNAP_BY_ORDINAL64(lookupValue)) {
                remoteFunction = caller.invoke(getProcAddress, remoteModule, IMAGE_ORDINAL64(lookupValue));
            } else {
                if (lookupValue > (std::numeric_limits<DWORD>::max)()) fail("import-by-name RVA is invalid");
                const std::size_t importRva = static_cast<std::size_t>(lookupValue);
                image.at<WORD>(importRva);
                const std::string functionName = image.stringAt(importRva + sizeof(WORD), 1024);
                if (functionName.empty()) fail("empty imported function name");
                remoteFunction = invokeWithString(caller, process, getProcAddress, functionName, uncertain, remoteModule);
            }
            if (remoteFunction == 0) fail("target could not resolve an imported function from " + moduleName);
            destination->u1.Function = remoteFunction;
        }
        if (!thunkTerminated) fail("import table is unterminated or exceeds the import limit");
    }
    if (!foundTerminator) fail("import descriptor table is not terminated");
}

DWORD sectionProtection(DWORD characteristics) {
    const bool executable = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool readable = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    const bool writable = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    if (executable && writable) fail("writable-executable section reached protection planning");
    DWORD protection = PAGE_NOACCESS;
    if (executable) protection = readable ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    else if (writable) protection = PAGE_READWRITE;
    else if (readable) protection = PAGE_READONLY;
    if (protection != PAGE_NOACCESS && (characteristics & IMAGE_SCN_MEM_NOT_CACHED) != 0) protection |= PAGE_NOCACHE;
    return protection;
}

void protectRemoteImage(
    HANDLE process,
    std::uintptr_t remoteBase,
    const ParsedImage& image,
    bool discardSections
) {
    DWORD oldProtection = 0;
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(remoteBase), image.optional.SizeOfImage, PAGE_NOACCESS, &oldProtection)) {
        failWin32("could not close unused image pages");
    }
    if (!VirtualProtectEx(process, reinterpret_cast<void*>(remoteBase), image.optional.SizeOfHeaders, PAGE_READONLY, &oldProtection)) {
        failWin32("could not protect mapped headers");
    }
    for (const auto& section : image.sections) {
        const std::size_t size = (std::max)(
            static_cast<std::size_t>(section.Misc.VirtualSize),
            static_cast<std::size_t>(section.SizeOfRawData)
        );
        if (size == 0) continue;
        DWORD protection = sectionProtection(section.Characteristics);
        if (discardSections && (section.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0) {
            protection = PAGE_NOACCESS;
        }
        if (!VirtualProtectEx(
                process,
                reinterpret_cast<void*>(remoteBase + section.VirtualAddress),
                size,
                protection,
                &oldProtection)) {
            failWin32("could not apply final section protections");
        }
    }
    if (!FlushInstructionCache(process, reinterpret_cast<void*>(remoteBase), image.optional.SizeOfImage)) {
        failWin32("could not flush the target instruction cache");
    }
}

} // namespace

ManualMapResult manualMapDllDetailed(
    std::uint32_t pid,
    const std::filesystem::path& dllPath,
    const ManualMapOptions& options
) {
#if !defined(_M_X64)
    (void)pid;
    (void)dllPath;
    (void)options;
    fail("manual mapping requires the x64 Wapi build");
#else
    Deadline deadline(options.timeoutMs == 0 ? 15000u : options.timeoutMs);
    if (dllPath.empty()) fail("DLL path is empty");
    if (!std::filesystem::is_regular_file(dllPath)) fail("DLL path does not exist: " + dllPath.string());
    ParsedImage image = parseImage(dllPath);
    if (deadline.remainingMs() == 0) fail("operation timed out during DLL parsing");

    ManualMapResult result;
    result.imageSize = image.optional.SizeOfImage;
    result.sectionCount = image.sections.size();
    result.relocationCount = validateRelocations(image);
    result.importModuleCount = validateImports(image);
    validateTls(image, static_cast<std::uintptr_t>(image.optional.ImageBase));
    validateExceptionDirectory(image);
    const auto validatedExceptionDirectory = image.directory(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (validatedExceptionDirectory.VirtualAddress != 0) {
        result.unwindEntryCount = validatedExceptionDirectory.Size / sizeof(X64RuntimeFunctionEntry);
    }
    result.validated = true;

    if (deadline.remainingMs() == 0) fail("operation timed out during DLL structural validation");

    constexpr DWORD queryRights = PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION;
    ScopedHandle queryProcess(OpenProcess(queryRights, FALSE, pid));
    if (!queryProcess) failWin32("could not open target process for validation");
    validateTarget(pid, queryProcess.get());
    if (deadline.remainingMs() == 0) fail("operation timed out during target validation");
    if (options.validateOnly) return result;

    constexpr DWORD processRights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE;
    ScopedHandle process(OpenProcess(processRights, FALSE, pid));
    if (!process) failWin32("could not open target process with mapper rights");
    if (deadline.remainingMs() == 0) fail("operation timed out before remote mapping could start");

    bool uncertain = false;
    void* remoteAddress = VirtualAllocEx(
        process.get(),
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(image.optional.ImageBase)),
        image.optional.SizeOfImage,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );
    if (!remoteAddress) {
        remoteAddress = VirtualAllocEx(
            process.get(), nullptr, image.optional.SizeOfImage,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE
        );
    }
    if (!remoteAddress) failWin32("could not allocate the remote image");
    RemoteAllocation remoteImage(process.get(), remoteAddress, uncertain);
    const std::uintptr_t remoteBase = remoteImage.value();

    result.baseAddress = remoteBase;
    applyRelocations(image, remoteBase);
    validateTls(image, remoteBase);

    RemoteCaller caller(process.get(), deadline, uncertain);
    const std::uintptr_t remoteLoadLibrary = remoteAddressForLocalFunction(
        pid, requiredProc(L"kernel32.dll", "LoadLibraryA")
    );
    const std::uintptr_t remoteGetProcAddress = remoteAddressForLocalFunction(
        pid, requiredProc(L"kernel32.dll", "GetProcAddress")
    );
    const std::uintptr_t remoteFreeLibrary = remoteAddressForLocalFunction(
        pid, requiredProc(L"kernel32.dll", "FreeLibrary")
    );
    DependencyRollback dependencies(caller, remoteFreeLibrary, uncertain);
    resolveImports(
        image, process.get(), caller, dependencies,
        remoteLoadLibrary, remoteGetProcAddress, uncertain
    );

    writeRemote(
        process.get(), remoteImage.get(), image.bytes.data(), image.bytes.size(),
        "could not write the mapped image"
    );
    protectRemoteImage(process.get(), remoteBase, image, false);

    std::unique_ptr<FunctionTableRollback> functionTableRollback;
    const auto exceptionDirectory = image.directory(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (exceptionDirectory.VirtualAddress != 0) {
        const std::size_t functionCount = exceptionDirectory.Size / sizeof(X64RuntimeFunctionEntry);
        const std::uintptr_t remoteAddFunctionTable = remoteAddressForLocalFunction(
            pid, requiredProc(L"ntdll.dll", "RtlAddFunctionTable")
        );
        const std::uintptr_t remoteDeleteFunctionTable = remoteAddressForLocalFunction(
            pid, requiredProc(L"ntdll.dll", "RtlDeleteFunctionTable")
        );
        const std::uintptr_t tableAddress = remoteBase + exceptionDirectory.VirtualAddress;
        if (caller.invoke(remoteAddFunctionTable, tableAddress, functionCount, remoteBase) == 0) {
            fail("RtlAddFunctionTable rejected the mapped exception directory");
        }
        result.unwindRegistered = true;
        functionTableRollback = std::make_unique<FunctionTableRollback>(
            caller, remoteDeleteFunctionTable, tableAddress, uncertain
        );
    }

    if (image.optional.AddressOfEntryPoint != 0) {
        const std::uintptr_t entryPoint = remoteBase + image.optional.AddressOfEntryPoint;
        if (caller.invoke(entryPoint, remoteBase, DLL_PROCESS_ATTACH, 0) == 0) {
            uncertain = true;
            fail("DLL entry point returned FALSE; target state is uncertain and allocations were retained");
        }
        result.entryPointCalled = true;
    }

    try {
        protectRemoteImage(process.get(), remoteBase, image, true);
    } catch (...) {
        uncertain = true;
        throw;
    }

    result.finalProtectionsApplied = true;
    if (functionTableRollback) functionTableRollback->release();
    dependencies.release();
    remoteImage.release();
    return result;
#endif
}

std::uintptr_t manualMapDll(
    std::uint32_t pid,
    const std::filesystem::path& dllPath,
    const ManualMapOptions& options
) {
    return manualMapDllDetailed(pid, dllPath, options).baseAddress;
}

} // namespace wapi::injection
