#include "evaluator.h"
#include <iostream>
#include <stdexcept>
#include <Windows.h>
#include <TlHelp32.h>

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
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);

    if (Process32First(snap, &entry)) {
        do {
            if (std::wstring(entry.szExeFile) == std::wstring(name.begin(), name.end()) + L".exe") {
                CloseHandle(snap);
                return (int)entry.th32ProcessID;
            }
        } while (Process32Next(snap, &entry));
    }

    CloseHandle(snap);
    return -1; // not found
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
        std::cout << "Failed to open process\n";
        return -1;
    }
    std::cout << "Opened process " << pid << " successfully\n";
    return (int)(uintptr_t)handle;
}

WapiValue Evaluator::wapi_terminateProcess(int pid) {
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess != NULL) {
        if (TerminateProcess(hProcess, 0)) {
			std::cout << "Terminated process " << pid << " successfully\n";
			CloseHandle(hProcess);
			return 0;
		}
        else {
            std::cout << "Failed to terminate process\n";
            CloseHandle(hProcess);
            return -1;
        }
	}
	else {
		std::cout << "Failed to open process for termination\n";
		return -1;
    }
}

WapiValue Evaluator::wapi_suspendProcess(int pid) {
    HANDLE hProcess = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (hProcess != NULL) {

    }
}