#include "evaluator.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

Evaluator::Evaluator(const WapiRuntimeOptions& options) : options(options) {}

void Evaluator::run(std::shared_ptr<Program> program) {
    for (auto& stmt : program->statements) {
        evalNode(stmt);
    }
}

WapiValue Evaluator::evalNode(std::shared_ptr<ASTNode> node) {
    if (auto n = std::dynamic_pointer_cast<IntLiteral>(node)) return n->value;
    if (auto n = std::dynamic_pointer_cast<LongLongLiteral>(node)) return n->value;
    if (auto n = std::dynamic_pointer_cast<StringLiteral>(node)) return n->value;

    if (auto n = std::dynamic_pointer_cast<Identifier>(node)) {
        if (variables.count(n->name)) return variables[n->name];
        throw std::runtime_error("E_UNDEFINED_VAR: " + n->name);
    }

    if (auto n = std::dynamic_pointer_cast<VarDeclaration>(node)) {
        WapiValue val = evalNode(n->value);
        variables[n->name] = val;
        return val;
    }

    if (auto n = std::dynamic_pointer_cast<FunctionCall>(node)) {
        return evalFunctionCall(n);
    }

    throw std::runtime_error("E_UNKNOWN_NODE: unsupported AST node");
}

void Evaluator::checkArgCount(const std::shared_ptr<FunctionCall>& call, size_t expected) {
    if (call->args.size() != expected) {
        throw std::runtime_error(
            "E_ARG_COUNT:" + call->name + " expected=" + std::to_string(expected) +
            " got=" + std::to_string(call->args.size())
        );
    }
}

std::string Evaluator::modeToString() const {
    switch (options.mode) {
    case WapiMode::Safe: return "safe";
    case WapiMode::Dev: return "dev";
    case WapiMode::Unsafe: return "unsafe";
    }
    return "safe";
}

std::string Evaluator::nowIso8601() const {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &t);
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

void Evaluator::emitAudit(const std::string& functionName, const std::string& capability, const std::string& result, const std::string& detail) const {
    std::cout
        << "[WAPI_AUDIT]"
        << " ts=" << nowIso8601()
        << " mode=" << modeToString()
        << " checkOnly=" << (options.checkOnly ? "true" : "false")
        << " function=" << functionName
        << " capability=" << capability
        << " result=" << result;

    if (!detail.empty()) {
        std::cout << " detail=\"" << detail << "\"";
    }
    std::cout << "\n";
}

void Evaluator::enforcePolicy(const std::string& functionName, const std::string& capability, bool requiresInjectionFlag) const {
    const bool hasCapability = options.capabilities.count(capability) > 0;

    if (requiresInjectionFlag && options.mode != WapiMode::Unsafe && !options.allowInjection) {
        emitAudit(functionName, capability, "deny", "--allow-injection is required outside unsafe mode");
        throw std::runtime_error("E_PERMISSION:" + functionName + " requires --allow-injection");
    }

    if (hasCapability) {
        emitAudit(functionName, capability, "allow", "capability_present");
        return;
    }

    if (options.strictPermissions) {
        emitAudit(functionName, capability, "deny", "missing capability in strict mode");
        throw std::runtime_error("E_PERMISSION:" + functionName + " missing capability=" + capability);
    }

    emitAudit(functionName, capability, "warn", "missing capability allowed for v0.1 compatibility");
    std::cout << "[WAPI_WARNING][E_PERMISSION_SOFT] " << functionName
              << " missing capability '" << capability
              << "' (allowed in v0.1 compatibility mode)\n";
}

[[noreturn]] void Evaluator::throwArgType(const std::string& functionName, int argIndex, const std::string& expectedType) const {
    throw std::runtime_error(
        "E_ARG_TYPE:" + functionName + " arg=" + std::to_string(argIndex) + " expected=" + expectedType
    );
}

std::string Evaluator::asString(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex) {
    WapiValue v = evalNode(node);
    if (auto p = std::get_if<std::string>(&v)) return *p;
    throwArgType(functionName, argIndex, "string");
}

int Evaluator::asInt(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex) {
    WapiValue v = evalNode(node);
    if (auto p = std::get_if<int>(&v)) return *p;
    throwArgType(functionName, argIndex, "int");
}

long long Evaluator::asLongLong(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex) {
    WapiValue v = evalNode(node);
    if (auto p = std::get_if<long long>(&v)) return *p;
    if (auto pInt = std::get_if<int>(&v)) return static_cast<long long>(*pInt);
    throwArgType(functionName, argIndex, "long|int");
}

void* Evaluator::requireTrackedHandle(long long handleValue, const std::string& functionName) {
    if (handleValue == 0) {
        throw std::runtime_error("E_HANDLE_INVALID:" + functionName + " handle=0");
    }
    if (!trackedHandles.count(handleValue)) {
        throw std::runtime_error("E_HANDLE_UNKNOWN:" + functionName + " handle_not_tracked");
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(handleValue));
}

WapiValue Evaluator::evalFunctionCall(std::shared_ptr<FunctionCall> call) {
    if (call->name == "findProcessPID") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.list");
        return wapi_findProcessPID(asString(call->args[0], call->name, 0));
    }
    if (call->name == "listProcesses") {
        checkArgCount(call, 0);
        enforcePolicy(call->name, "proc.list");
        return wapi_listProcesses();
    }
    if (call->name == "openProcess") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.open.all_access");
        return wapi_openProcess(asInt(call->args[0], call->name, 0));
    }
    if (call->name == "terminateProcess") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.terminate");
        return wapi_terminateProcess(asLongLong(call->args[0], call->name, 0));
    }
    if (call->name == "suspendProcess") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.suspend");
        return wapi_suspendProcess(asLongLong(call->args[0], call->name, 0));
    }
    if (call->name == "resumeProcess") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.resume");
        return wapi_resumeProcess(asLongLong(call->args[0], call->name, 0));
    }
    if (call->name == "closeProcess") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.close");
        return wapi_closeProcess(asLongLong(call->args[0], call->name, 0));
    }



    if (call->name == "readMemory") {
        checkArgCount(call, 2);
        enforcePolicy(call->name, "mem.read");
        return wapi_readMemory(
            asLongLong(call->args[0], call->name, 0),
            asLongLong(call->args[1], call->name, 1)
        );
    }
    if (call->name == "writeMemory") {
        checkArgCount(call, 3);
        enforcePolicy(call->name, "mem.write");
        return wapi_writeMemory(
            asLongLong(call->args[0], call->name, 0),
            asLongLong(call->args[1], call->name, 1),
            asInt(call->args[2], call->name, 2)
        );
    }
    if (call->name == "allocMemory") {
        checkArgCount(call, 2);
        enforcePolicy(call->name, "mem.alloc");
        return wapi_allocMemory(
            asLongLong(call->args[0], call->name, 0),
            asInt(call->args[1], call->name, 1)
        );
    }
    if (call->name == "freeMemory") {
        checkArgCount(call, 2);
        enforcePolicy(call->name, "mem.free");
        return wapi_freeMemory(
            asLongLong(call->args[0], call->name, 0),
            asLongLong(call->args[1], call->name, 1)
        );
    }



    if (call->name == "listModules") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.modules");
        return wapi_listModules(asInt(call->args[0], call->name, 0));
    }



    if (call->name == "closeHandle") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "proc.handle.close");
        return wapi_closeHandle(asLongLong(call->args[0], call->name, 0));
    }



    if (call->name == "findWindow") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "window.find");
        return wapi_findWindow(asString(call->args[0], call->name, 0));
    }



    if (call->name == "injectDLL") {
        checkArgCount(call, 2);
        enforcePolicy(call->name, "inject.dll", true);
        return wapi_injectDLL(
            asInt(call->args[0], call->name, 0),
            asString(call->args[1], call->name, 1)
        );
    }
    if (call->name == "testInjectDLL") {
        checkArgCount(call, 1);
        enforcePolicy(call->name, "inject.dll", true);
        return wapi_testInjectDLL(asInt(call->args[0], call->name, 0));
    }

    throw std::runtime_error("E_UNKNOWN_FUNCTION:" + call->name);
}


/*
██╗███╗   ███╗██████╗ ██╗     ███████╗███╗   ███╗███████╗███╗   ██╗████████╗ █████╗ ████████╗██╗ ██████╗ ███╗   ██╗
██║████╗ ████║██╔══██╗██║     ██╔════╝████╗ ████║██╔════╝████╗  ██║╚══██╔══╝██╔══██╗╚══██╔══╝██║██╔═══██╗████╗  ██║
██║██╔████╔██║██████╔╝██║     █████╗  ██╔████╔██║█████╗  ██╔██╗ ██║   ██║   ███████║   ██║   ██║██║   ██║██╔██╗ ██║
██║██║╚██╔╝██║██╔═══╝ ██║     ██╔══╝  ██║╚██╔╝██║██╔══╝  ██║╚██╗██║   ██║   ██╔══██║   ██║   ██║██║   ██║██║╚██╗██║
██║██║ ╚═╝ ██║██║     ███████╗███████╗██║ ╚═╝ ██║███████╗██║ ╚████║   ██║   ██║  ██║   ██║   ██║╚██████╔╝██║ ╚████║
╚═╝╚═╝     ╚═╝╚═╝     ╚══════╝╚══════╝╚═╝     ╚═╝╚══════╝╚═╝  ╚═══╝   ╚═╝   ╚═╝  ╚═╝   ╚═╝   ╚═╝ ╚═════╝ ╚═╝  ╚═══╝


██████╗ ██████╗  ██████╗  ██████╗███████╗███████╗███████╗
██╔══██╗██╔══██╗██╔═══██╗██╔════╝██╔════╝██╔════╝██╔════╝
██████╔╝██████╔╝██║   ██║██║     █████╗  ███████╗███████╗
██╔═══╝ ██╔══██╗██║   ██║██║     ██╔══╝  ╚════██║╚════██║
██║     ██║  ██║╚██████╔╝╚██████╗███████╗███████║███████║
╚═╝     ╚═╝  ╚═╝ ╚═════╝  ╚═════╝╚══════╝╚══════╝╚══════╝

*/

WapiValue Evaluator::wapi_findProcessPID(const std::string& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) throw WapiUnstableException("Snapshot failed");

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    std::wstring target(name.begin(), name.end());
    target += L".exe";
    std::transform(target.begin(), target.end(), target.begin(), ::towlower);

    if (Process32FirstW(snap, &entry)) {
        do {
            std::wstring current(entry.szExeFile);
            std::transform(current.begin(), current.end(), current.begin(), ::towlower);
            if (current == target) {
                CloseHandle(snap);
                return static_cast<int>(entry.th32ProcessID);
            }
        } while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    throw WapiUnstableException("Process not found: " + name);
}

WapiValue Evaluator::wapi_listProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) throw WapiUnstableException("Snapshot failed");

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snap, &entry)) {
        do {
            std::wstring ws(entry.szExeFile);
            std::string name(ws.begin(), ws.end());
            std::cout << entry.th32ProcessID << " - " << name << "\n";
        } while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return 0;
}

WapiValue Evaluator::wapi_openProcess(int pid) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");

    if (options.checkOnly) {
        emitAudit("openProcess", "proc.open.all_access", "allow", "checkOnly no side-effects");
        const long long synthetic = 1;
        trackedHandles.insert(synthetic);
        return synthetic;
    }

    HANDLE handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) throw WapiUnstableException("Failed to open process");

    long long tracked = static_cast<long long>(reinterpret_cast<uintptr_t>(handle));
    trackedHandles.insert(tracked);
    return tracked;
}

WapiValue Evaluator::wapi_terminateProcess(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "terminateProcess"));

    if (options.checkOnly) {
        emitAudit("terminateProcess", "proc.terminate", "allow", "checkOnly no side-effects");
        return 0;
    }

    if (!TerminateProcess(hProcess, 0)) throw WapiUnstableException("Failed to terminate process");

    CloseHandle(hProcess);
    trackedHandles.erase(handle);
    return 0;
}

WapiValue Evaluator::wapi_suspendProcess(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "suspendProcess"));

    if (options.checkOnly) {
        emitAudit("suspendProcess", "proc.suspend", "allow", "checkOnly no side-effects");
        return 0;
    }

    using NtSuspendProcessFunc = LONG(NTAPI*)(HANDLE);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) throw WapiUnstableException("Failed to get ntdll.dll");

    auto NtSuspendProcess = reinterpret_cast<NtSuspendProcessFunc>(GetProcAddress(ntdll, "NtSuspendProcess"));
    if (!NtSuspendProcess) throw WapiUnstableException("Failed to find NtSuspendProcess");

    LONG status = NtSuspendProcess(hProcess);
    if (status != 0) throw WapiUnstableException("NtSuspendProcess failed");
    return 0;
}

WapiValue Evaluator::wapi_resumeProcess(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "resumeProcess"));

    if (options.checkOnly) {
        emitAudit("resumeProcess", "proc.resume", "allow", "checkOnly no side-effects");
        return 0;
    }

    using NtResumeProcessFunc = LONG(NTAPI*)(HANDLE);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) throw WapiUnstableException("Failed to get ntdll.dll");

    auto NtResumeProcess = reinterpret_cast<NtResumeProcessFunc>(GetProcAddress(ntdll, "NtResumeProcess"));
    if (!NtResumeProcess) throw WapiUnstableException("Failed to find NtResumeProcess");

    LONG status = NtResumeProcess(hProcess);
    if (status != 0) throw WapiUnstableException("NtResumeProcess failed");
    return 0;
}

WapiValue Evaluator::wapi_closeProcess(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "closeProcess"));

    if (options.checkOnly) {
        emitAudit("closeProcess", "proc.close", "allow", "checkOnly no side-effects");
        trackedHandles.erase(handle);
        return 0;
    }

    CloseHandle(hProcess);
    trackedHandles.erase(handle);
    return 0;
}

/*
███╗   ███╗███████╗███╗   ███╗ ██████╗ ██████╗ ██╗   ██╗
████╗ ████║██╔════╝████╗ ████║██╔═══██╗██╔══██╗╚██╗ ██╔╝
██╔████╔██║█████╗  ██╔████╔██║██║   ██║██████╔╝ ╚████╔╝
██║╚██╔╝██║██╔══╝  ██║╚██╔╝██║██║   ██║██╔══██╗  ╚██╔╝
██║ ╚═╝ ██║███████╗██║ ╚═╝ ██║╚██████╔╝██║  ██║   ██║
╚═╝     ╚═╝╚══════╝╚═╝     ╚═╝ ╚═════╝ ╚═╝  ╚═╝   ╚═╝
*/

WapiValue Evaluator::wapi_readMemory(long long handle, long long address) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "readMemory"));
    if (address <= 0) throw WapiUnstableException("Invalid memory address");

    if (options.checkOnly) {
        emitAudit("readMemory", "mem.read", "allow", "checkOnly no side-effects");
        return 0;
    }

    int buffer = 0;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &buffer, sizeof(buffer), &bytesRead)) {
        throw WapiUnstableException("Failed to read process memory");
    }

    std::cout << "Read " << bytesRead << " bytes from address 0x" << std::hex << address << ": " << std::dec << buffer << "\n";
    return buffer;
}

WapiValue Evaluator::wapi_writeMemory(long long handle, long long address, int value) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "writeMemory"));
    if (address <= 0) throw WapiUnstableException("Invalid memory address");

    if (options.checkOnly) {
        emitAudit("writeMemory", "mem.write", "allow", "checkOnly no side-effects");
        return 0;
    }

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), &value, sizeof(value), &bytesWritten)) {
        throw WapiUnstableException("Failed to write process memory");
    }

    std::cout << "Wrote " << bytesWritten << " bytes to address 0x" << std::hex << address << "\n";
    return 0;
}

WapiValue Evaluator::wapi_allocMemory(long long handle, int size) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "allocMemory"));
    if (size <= 0) throw WapiUnstableException("Invalid allocation size");

    if (options.checkOnly) {
        emitAudit("allocMemory", "mem.alloc", "allow", "checkOnly no side-effects");
        return static_cast<long long>(0x1000);
    }

    LPVOID addr = VirtualAllocEx(hProcess, nullptr, static_cast<SIZE_T>(size), MEM_COMMIT, PAGE_READWRITE);
    if (!addr) throw WapiUnstableException("Failed to allocate memory");

    std::cout << "Allocated " << size << " bytes\n";
    return static_cast<long long>(reinterpret_cast<uintptr_t>(addr));
}

WapiValue Evaluator::wapi_freeMemory(long long handle, long long address) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "freeMemory"));
    if (address <= 0) throw WapiUnstableException("Invalid memory address");

    if (options.checkOnly) {
        emitAudit("freeMemory", "mem.free", "allow", "checkOnly no side-effects");
        return 0;
    }

    if (!VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(address)), 0, MEM_RELEASE)) {
        throw WapiUnstableException("Failed to free memory");
    }

    std::cout << "Freed memory\n";
    return 0;
}

/*
███╗   ███╗ ██████╗ ██████╗ ██╗   ██╗██╗     ███████╗███████╗
████╗ ████║██╔═══██╗██╔══██╗██║   ██║██║     ██╔════╝██╔════╝
██╔████╔██║██║   ██║██║  ██║██║   ██║██║     █████╗  ███████╗
██║╚██╔╝██║██║   ██║██║  ██║██║   ██║██║     ██╔══╝  ╚════██║
██║ ╚═╝ ██║╚██████╔╝██████╔╝╚██████╔╝███████╗███████╗███████║
╚═╝     ╚═╝ ╚═════╝ ╚═════╝  ╚═════╝ ╚══════╝╚══════╝╚══════╝
*/
WapiValue Evaluator::wapi_listModules(int pid) {
    // 1. Initialize your custom return variable at the start
    WapiValue result;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);

    if (hSnapshot == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to create snapshot. Error: " << GetLastError() << std::endl;
        return result; // Fix: Must return WapiValue, not empty return;
    }

    MODULEENTRY32W me32;
    me32.dwSize = sizeof(MODULEENTRY32W);

    if (!Module32FirstW(hSnapshot, &me32)) {
        std::cerr << "Failed to get first module. Error: " << GetLastError() << std::endl;
        CloseHandle(hSnapshot);
        return result; // Fix: Must return WapiValue, not empty return;
    }

    do {
        // TODO: Populate 'result' with 'me32.szModule' and 'me32.szExePath'
        // Example if WapiValue has an append method:
        // result.append(me32.szModule);
    } while (Module32NextW(hSnapshot, &me32));

    CloseHandle(hSnapshot);

    return result; // Fix: Return the populated object instead of 0
}



/*
██╗  ██╗ █████╗ ███╗   ██╗██████╗ ██╗     ███████╗    ███╗   ███╗ █████╗ ███╗   ██╗ █████╗  ██████╗ ███████╗███╗   ███╗███████╗███╗   ██╗████████╗
██║  ██║██╔══██╗████╗  ██║██╔══██╗██║     ██╔════╝    ████╗ ████║██╔══██╗████╗  ██║██╔══██╗██╔════╝ ██╔════╝████╗ ████║██╔════╝████╗  ██║╚══██╔══╝
███████║███████║██╔██╗ ██║██║  ██║██║     █████╗      ██╔████╔██║███████║██╔██╗ ██║███████║██║  ███╗█████╗  ██╔████╔██║█████╗  ██╔██╗ ██║   ██║
██╔══██║██╔══██║██║╚██╗██║██║  ██║██║     ██╔══╝      ██║╚██╔╝██║██╔══██║██║╚██╗██║██╔══██║██║   ██║██╔══╝  ██║╚██╔╝██║██╔══╝  ██║╚██╗██║   ██║
██║  ██║██║  ██║██║ ╚████║██████╔╝███████╗███████╗    ██║ ╚═╝ ██║██║  ██║██║ ╚████║██║  ██║╚██████╔╝███████╗██║ ╚═╝ ██║███████╗██║ ╚████║   ██║
╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═══╝╚═════╝ ╚══════╝╚══════╝    ╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═══╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝     ╚═╝╚══════╝╚═╝  ╚═══╝   ╚═╝
*/

WapiValue Evaluator::wapi_closeHandle(long long handle) {
    HANDLE hProcess = reinterpret_cast<HANDLE>(requireTrackedHandle(handle, "closeHandle"));

    if (options.checkOnly) {
        emitAudit("closeHandle", "proc.open.all_access", "allow", "checkOnly no side-effects");
        trackedHandles.erase(handle);
        return 0;
    }

    CloseHandle(hProcess);
    trackedHandles.erase(handle);
    return 0;
}

/*
██╗    ██╗██╗███╗   ██╗██████╗  ██████╗ ██╗    ██╗
██║    ██║██║████╗  ██║██╔══██╗██╔═══██╗██║    ██║
██║ █╗ ██║██║██╔██╗ ██║██║  ██║██║   ██║██║ █╗ ██║
██║███╗██║██║██║╚██╗██║██║  ██║██║   ██║██║███╗██║
╚███╔███╔╝██║██║ ╚████║██████╔╝╚██████╔╝╚███╔███╔╝
 ╚══╝╚══╝ ╚═╝╚═╝  ╚═══╝╚═════╝  ╚═════╝  ╚══╝╚══╝
*/

WapiValue Evaluator::wapi_findWindow(const std::string& name) {
    if (options.checkOnly) {
        emitAudit("findWindow", "window.find", "allow", "checkOnly no side-effects");
    }

    HWND hwnd = FindWindowA(nullptr, name.c_str());
    if (!hwnd) return 0;
    return static_cast<long long>(reinterpret_cast<uintptr_t>(hwnd));
}

/*
██████╗ ██╗     ██╗         ██╗███╗   ██╗     ██╗███████╗ ██████╗████████╗██╗ ██████╗ ███╗   ██╗
██╔══██╗██║     ██║         ██║████╗  ██║     ██║██╔════╝██╔════╝╚══██╔══╝██║██╔═══██╗████╗  ██║
██║  ██║██║     ██║         ██║██╔██╗ ██║     ██║█████╗  ██║        ██║   ██║██║   ██║██╔██╗ ██║
██║  ██║██║     ██║         ██║██║╚██╗██║██   ██║██╔══╝  ██║        ██║   ██║██║   ██║██║╚██╗██║
██████╔╝███████╗███████╗    ██║██║ ╚████║╚█████╔╝███████╗╚██████╗   ██║   ██║╚██████╔╝██║ ╚████║
╚═════╝ ╚══════╝╚══════╝    ╚═╝╚═╝  ╚═══╝ ╚════╝ ╚══════╝ ╚═════╝   ╚═╝   ╚═╝ ╚═════╝ ╚═╝  ╚═══╝
*/

WapiValue Evaluator::wapi_injectDLL(int pid, const std::string& dllPath) {
    if (pid <= 0) throw WapiUnstableException("Invalid pid");
    if (dllPath.empty()) throw WapiUnstableException("DLL path is empty");
    if (!std::filesystem::exists(std::filesystem::path(dllPath))) {
        throw WapiUnstableException("DLL path does not exist: " + dllPath);
    }

    if (options.checkOnly) {
        emitAudit("injectDLL", "inject.dll", "allow", "checkOnly no side-effects");
        return 0;
    }

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(pid));
    if (!hProcess) throw WapiUnstableException("Failed to open process");

    LPVOID remote = VirtualAllocEx(hProcess, nullptr, dllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to allocate memory");
    }

    if (!WriteProcessMemory(hProcess, remote, dllPath.c_str(), dllPath.size() + 1, nullptr)) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to write memory");
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA")), remote, 0, nullptr);

    if (!hThread) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to create remote thread");
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    if (exitCode == 0) {
        throw WapiUnstableException("LoadLibrary failed inside target process - check DLL path");
    }

    std::cout << "DLL injected successfully\n";
    return 0;
}

WapiValue Evaluator::wapi_testInjectDLL(int pid) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring dllPathW = dir + L"\\TestDLL.dll";
    std::string dllPath(dllPathW.begin(), dllPathW.end());

    return wapi_injectDLL(pid, dllPath);
}

