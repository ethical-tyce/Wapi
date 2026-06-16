#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include "parser.h"

using WapiValue = std::variant<int, long long, std::string, bool>;

enum class WapiMode {
    Safe,
    Dev,
    Unsafe
};

struct WapiRuntimeOptions {
    WapiMode mode = WapiMode::Safe;
    bool allowInjection = false;
    bool checkOnly = false;
    bool strictPermissions = false;
    std::unordered_set<std::string> capabilities;
};

class WapiUnstableException : public std::exception {
public:
    explicit WapiUnstableException(const std::string& msg) : msg(msg) {}
    const char* what() const noexcept override { return msg.c_str(); }
private:
    std::string msg;
};

class Evaluator {
public:
    explicit Evaluator(const WapiRuntimeOptions& options = {});
    ~Evaluator();
    void run(std::shared_ptr<Program> program);

private:
    WapiRuntimeOptions options;
    std::unordered_map<std::string, WapiValue> variables;
    std::unordered_set<long long> trackedHandles;
    std::unordered_map<long long, std::unordered_set<long long>> trackedAllocations;

    WapiValue evalNode(std::shared_ptr<ASTNode> node);
    WapiValue evalFunctionCall(std::shared_ptr<FunctionCall> call);
    WapiValue evalUnaryExpression(const UnaryExpression& expr);
    WapiValue evalBinaryExpression(const BinaryExpression& expr);

    void checkArgCount(const std::shared_ptr<FunctionCall>& call, size_t expected);
    std::string modeToString() const;
    std::string nowIso8601() const;
    std::string valueToString(const WapiValue& value) const;
    bool isTruthy(const WapiValue& value) const;
    long long asNumberValue(const WapiValue& value, const std::string& context) const;
    void emitAudit(const std::string& functionName, const std::string& capability, const std::string& result, const std::string& detail = "") const;
    void enforcePolicy(const std::string& functionName, const std::string& capability, bool requiresInjectionFlag = false) const;
    [[noreturn]] void throwArgType(const std::string& functionName, int argIndex, const std::string& expectedType) const;

    std::string asString(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex);
    int asInt(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex);
    long long asLongLong(const std::shared_ptr<ASTNode>& node, const std::string& functionName, int argIndex);

    void* requireTrackedHandle(long long handleValue, const std::string& functionName);
    void cleanupTrackedResources() noexcept;
    void releaseTrackedAllocations(long long handleValue) noexcept;
    void closeTrackedHandle(long long handleValue) noexcept;

    // Windows API bindings
    WapiValue wapi_findProcessPID(const std::string& name);
    WapiValue wapi_listProcesses();
    WapiValue wapi_openProcess(int pid);
    WapiValue wapi_terminateProcess(long long handle);
    WapiValue wapi_suspendProcess(long long handle);
    WapiValue wapi_resumeProcess(long long handle);
    WapiValue wapi_closeProcess(long long handle);


    WapiValue wapi_readMemory(long long handle, long long address);
    WapiValue wapi_writeMemory(long long handle, long long address, int value);
    WapiValue wapi_allocMemory(long long handle, int size);
    WapiValue wapi_freeMemory(long long handle, long long address);


    WapiValue wapi_listModules(int pid);
	WapiValue wapi_getModuleBaseAddress(int pid, const std::string& moduleName);



    WapiValue wapi_closeHandle(long long handle);



    WapiValue wapi_findWindow(const std::string& name);



    WapiValue wapi_injectDLL(int pid, const std::string& dllPath);
    WapiValue wapi_testInjectDLL(int pid);
};
