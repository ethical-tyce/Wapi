#pragma once
#include <string>
#include <unordered_map>
#include <variant>
#include "parser.h"

using WapiValue = std::variant<int, long long, std::string, bool>;

class WapiUnstableException : public std::exception {
public:
    WapiUnstableException(const std::string& msg) : msg(msg) {}
    const char* what() const noexcept override { return msg.c_str(); }
private:
    std::string msg;
};

class Evaluator {
public:
    void run(std::shared_ptr<Program> program);

private:
    std::unordered_map<std::string, WapiValue> variables;

    WapiValue evalNode(std::shared_ptr<ASTNode> node);
    WapiValue evalFunctionCall(std::shared_ptr<FunctionCall> call);

    // Windows API bindings
    WapiValue wapi_findProcessPID(const std::string& name);
    WapiValue wapi_listProcesses();
    WapiValue wapi_openProcess(int pid);
    WapiValue wapi_terminateProcess(long long handle);
    WapiValue wapi_suspendProcess(long long handle);
    WapiValue wapi_resumeProcess(long long handle);
    WapiValue wapi_readMemory(long long handle, long long address);
    WapiValue wapi_writeMemory(long long handle, long long address);
    

    WapiValue wapi_injectDLL(int pid, const std::string& dllPath);



};