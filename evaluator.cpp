#include "evaluator.h"
#include <iostream>
#include <stdexcept>
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>

void Evaluator::run(std::shared_ptr<Program> program) {
    for (auto& stmt : program->statements)
        evalNode(stmt);
}

WapiValue Evaluator::evalNode(std::shared_ptr<ASTNode> node) {
    if (auto n = std::dynamic_pointer_cast<IntLiteral>(node))
        return n->value;

    if (auto n = std::dynamic_pointer_cast<LongLongLiteral>(node))
        return n->value;

    if (auto n = std::dynamic_pointer_cast<StringLiteral>(node))
        return n->value;

    if (auto n = std::dynamic_pointer_cast<Identifier>(node)) {
        if (variables.count(n->name)) return variables[n->name];
        throw std::runtime_error("Undefined variable: " + n->name);
    }

    if (auto n = std::dynamic_pointer_cast<VarDeclaration>(node)) {
        WapiValue val = evalNode(n->value);
        variables[n->name] = val;
        return val;
    }

    if (auto n = std::dynamic_pointer_cast<FunctionCall>(node))
        return evalFunctionCall(n);

    throw std::runtime_error("Unknown node");
}

WapiValue Evaluator::evalFunctionCall(std::shared_ptr<FunctionCall> call) {

    // PROCESS

    if (call->name == "findProcessPID") {
        std::string name = std::get<std::string>(evalNode(call->args[0]));
        return wapi_findProcessPID(name);
    }
    if (call->name == "listProcesses") {
        return wapi_listProcesses();
    }
    if (call->name == "openProcess") {
        int pid = std::get<int>(evalNode(call->args[0]));
        return wapi_openProcess(pid);
    }
    if (call->name == "terminateProcess") {
        long long handle = std::get<long long>(evalNode(call->args[0]));
        return wapi_terminateProcess(handle);
    }
    if (call->name == "suspendProcess") {
        long long handle = std::get<long long>(evalNode(call->args[0]));
        return wapi_suspendProcess(handle);
    }
    if (call->name == "resumeProcess") {
        long long handle = std::get<long long>(evalNode(call->args[0]));
        return wapi_resumeProcess(handle);
    }

    // MEMORY

    if (call->name == "readMemory") {
        long long handle = std::get<long long>(evalNode(call->args[0]));
        long long address = std::get<long long>(evalNode(call->args[1]));
        return wapi_readMemory(handle, address);
    }
    if (call->name == "writeMemory") {
        long long handle = std::get<long long>(evalNode(call->args[0]));
        long long address = std::get<long long>(evalNode(call->args[1]));
        int value = std::get<int>(evalNode(call->args[2]));
        return wapi_writeMemory(handle, address, value);
    }
    if (call->name == "allocMemory") {
        long long handle = std::get<long long>(evalNode(call->args[0]));
        int size = std::get<int>(evalNode(call->args[1]));
        return wapi_allocMemory(handle, size);
    }
    if (call->name == "freeMemory") {
        long long handle = std::get<long long>(evalNode(call->args[0]));
        long long address = std::get<long long>(evalNode(call->args[1]));
        return wapi_freeMemory(handle, address);
    }

    // WINDOW

    if (call->name == "findWindow") {
        std::string name = std::get<std::string>(evalNode(call->args[0]));
        return wapi_findWindow(name);
    }

    // DLL INJECTION

    if (call->name == "injectDLL") {
        int pid = std::get<int>(evalNode(call->args[0]));
        std::string dllPath = std::get<std::string>(evalNode(call->args[1]));
        return wapi_injectDLL(pid, dllPath);
    }
    if (call->name == "testInjectDLL") {
        int pid = std::get<int>(evalNode(call->args[0]));
		return wapi_testInjectDLL(pid);
    }

    throw std::runtime_error("Unknown function: " + call->name);
}

/*
██╗    ██╗██╗███╗   ██╗██████╗  ██████╗ ██╗    ██╗███████╗     █████╗ ██████╗ ██╗    ██████╗ ██╗███╗   ██╗██████╗ ██╗███╗   ██╗ ██████╗ ███████╗
██║    ██║██║████╗  ██║██╔══██╗██╔═══██╗██║    ██║██╔════╝    ██╔══██╗██╔══██╗██║    ██╔══██╗██║████╗  ██║██╔══██╗██║████╗  ██║██╔════╝ ██╔════╝
██║ █╗ ██║██║██╔██╗ ██║██║  ██║██║   ██║██║ █╗ ██║███████╗    ███████║██████╔╝██║    ██████╔╝██║██╔██╗ ██║██║  ██║██║██╔██╗ ██║██║  ███╗███████╗
██║███╗██║██║██║╚██╗██║██║  ██║██║   ██║██║███╗██║╚════██║    ██╔══██║██╔═══╝ ██║    ██╔══██╗██║██║╚██╗██║██║  ██║██║██║╚██╗██║██║   ██║╚════██║
╚███╔███╔╝██║██║ ╚████║██████╔╝╚██████╔╝╚███╔███╔╝███████║    ██║  ██║██║     ██║    ██████╔╝██║██║ ╚████║██████╔╝██║██║ ╚████║╚██████╔╝███████║
 ╚══╝╚══╝ ╚═╝╚═╝  ╚═══╝╚═════╝  ╚═════╝  ╚══╝╚══╝ ╚══════╝    ╚═╝  ╚═╝╚═╝     ╚═╝    ╚═════╝ ╚═╝╚═╝  ╚═══╝╚═════╝ ╚═╝╚═╝  ╚═══╝ ╚═════╝ ╚══════╝


██████╗ ██████╗  ██████╗  ██████╗███████╗███████╗███████╗
██╔══██╗██╔══██╗██╔═══██╗██╔════╝██╔════╝██╔════╝██╔════╝
██████╔╝██████╔╝██║   ██║██║     █████╗  ███████╗███████╗
██╔═══╝ ██╔══██╗██║   ██║██║     ██╔══╝  ╚════██║╚════██║
██║     ██║  ██║╚██████╔╝╚██████╗███████╗███████║███████║
╚═╝     ╚═╝  ╚═╝ ╚═════╝  ╚═════╝╚══════╝╚══════╝╚══════╝
*/

WapiValue Evaluator::wapi_findProcessPID(const std::string& name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        throw WapiUnstableException("Snapshot failed");

    PROCESSENTRY32W entry;
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
                return (int)entry.th32ProcessID;
            }
        } while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    throw WapiUnstableException("Process not found: " + name);
}

WapiValue Evaluator::wapi_listProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry;
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
    HANDLE handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (handle == NULL)
        throw WapiUnstableException("Failed to open process");
    return (long long)(uintptr_t)handle;
}

WapiValue Evaluator::wapi_terminateProcess(long long handle) {
    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    if (!TerminateProcess(hProcess, 0))
        throw WapiUnstableException("Failed to terminate process");
    CloseHandle(hProcess);
    return 0;
}

WapiValue Evaluator::wapi_suspendProcess(long long handle) {
    typedef LONG(NTAPI* NtSuspendProcessFunc)(HANDLE);

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) throw WapiUnstableException("Failed to get ntdll.dll");

    NtSuspendProcessFunc NtSuspendProcess = (NtSuspendProcessFunc)GetProcAddress(ntdll, "NtSuspendProcess");
    if (!NtSuspendProcess) throw WapiUnstableException("Failed to find NtSuspendProcess");

    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    LONG status = NtSuspendProcess(hProcess);

    if (status != 0) throw WapiUnstableException("NtSuspendProcess failed");

    return 0;
}

WapiValue Evaluator::wapi_resumeProcess(long long handle) {
    typedef LONG(NTAPI* NtResumeProcessFunc)(HANDLE);

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) throw WapiUnstableException("Failed to get ntdll.dll");

    NtResumeProcessFunc NtResumeProcess = (NtResumeProcessFunc)GetProcAddress(ntdll, "NtResumeProcess");
    if (!NtResumeProcess) throw WapiUnstableException("Failed to find NtResumeProcess");

    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    LONG status = NtResumeProcess(hProcess);

    if (status != 0) throw WapiUnstableException("NtResumeProcess failed");

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
    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    int buffer = 0;
    SIZE_T bytesRead;

    if (!ReadProcessMemory(hProcess, (LPCVOID)(uintptr_t)address, &buffer, sizeof(buffer), &bytesRead))
        throw WapiUnstableException("Failed to read process memory");

    std::cout << "Read " << bytesRead << " bytes from address 0x" << std::hex << address << ": " << std::dec << buffer << "\n";
    return buffer;
}

WapiValue Evaluator::wapi_writeMemory(long long handle, long long address, int value) {
    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    SIZE_T bytesWritten;

    if (!WriteProcessMemory(hProcess, (LPVOID)(uintptr_t)address, &value, sizeof(value), &bytesWritten))
        throw WapiUnstableException("Failed to write process memory");

    std::cout << "Wrote " << bytesWritten << " bytes to address 0x" << std::hex << address << "\n";
    return 0;
}

WapiValue Evaluator::wapi_allocMemory(long long handle, int size) {
    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    LPVOID addr = VirtualAllocEx(hProcess, NULL, size, MEM_COMMIT, PAGE_READWRITE);
    if (!addr) throw WapiUnstableException("Failed to allocate memory");
    std::cout << "Allocated " << size << " bytes\n";
    return (long long)(uintptr_t)addr;
}

WapiValue Evaluator::wapi_freeMemory(long long handle, long long address) {
    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    if (!VirtualFreeEx(hProcess, (LPVOID)(uintptr_t)address, 0, MEM_RELEASE))
        throw WapiUnstableException("Failed to free memory");
    std::cout << "Freed memory\n";
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
    return 0;
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
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) throw WapiUnstableException("Failed to open process");

    LPVOID remote = VirtualAllocEx(hProcess, NULL, dllPath.size() + 1, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to allocate memory");
    }

    if (!WriteProcessMemory(hProcess, remote, dllPath.c_str(), dllPath.size() + 1, NULL)) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to write memory");
    }

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
        remote, 0, NULL);

    if (!hThread) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        throw WapiUnstableException("Failed to create remote thread");
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode;
    GetExitCodeThread(hThread, &exitCode);
    std::cout << "LoadLibrary returned: " << exitCode << "\n";

    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    if (exitCode == 0)
        throw WapiUnstableException("LoadLibrary failed inside target process - check DLL path");

    std::cout << "DLL injected successfully\n";
    return 0;
}

WapiValue Evaluator::wapi_testInjectDLL(int pid) {
    // get the directory of Wapi.exe and look for TestDLL.dll there
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring dllPathW = dir + L"\\TestDLL.dll";
    std::string dllPath(dllPathW.begin(), dllPathW.end());

    std::cout << "Injecting: " << dllPath << "\n";
    return wapi_injectDLL(pid, dllPath);
}

