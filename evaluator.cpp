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
        int pid = std::get<int>(evalNode(call->args[0]));
        return wapi_terminateProcess(pid);
    }
    if (call->name == "suspendProcess") {
        int pid = std::get<int>(evalNode(call->args[0]));
        return wapi_suspendProcess(pid);
    }
	if (call->name == "resumeProcess") {
		int pid = std::get<int>(evalNode(call->args[0]));
		return wapi_resumeProcess(pid);
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
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);

    if (Process32First(snap, &entry)) {
        do {
            std::wstring ws(entry.szExeFile);
            std::string name(ws.begin(), ws.end());
            std::cout << entry.th32ProcessID << " - " << name << "\n";
        } while (Process32Next(snap, &entry));
    }

    CloseHandle(snap);
    return 0;
}

WapiValue Evaluator::wapi_openProcess(int pid) {
    HANDLE handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (handle == NULL) {
        throw WapiUnstableException("Failed to open process");
    }
    std::cout << "Opened process " << pid << " successfully\n";
    return (int)(uintptr_t)handle;
}

WapiValue Evaluator::wapi_terminateProcess(int handle) {
    HANDLE hProcess = (HANDLE)(uintptr_t)handle;
    if (!TerminateProcess(hProcess, 0))
        throw WapiUnstableException("Failed to terminate process");
    CloseHandle(hProcess);
    return 0;
}

WapiValue Evaluator::wapi_suspendProcess(int handle) {
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

WapiValue Evaluator::wapi_resumeProcess(int handle) {
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

