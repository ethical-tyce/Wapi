#pragma once
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include "parser.h"

struct WapiArray;
struct WapiStructInstance;
using WapiArrayPtr = std::shared_ptr<WapiArray>;
using WapiStructPtr = std::shared_ptr<WapiStructInstance>;
using WapiValue = std::variant<std::monostate, int, long long, double, std::string, bool, WapiArrayPtr, WapiStructPtr>;
struct WapiArray { std::vector<WapiValue> values; };
struct WapiStructInstance { std::string typeName; std::unordered_map<std::string, WapiValue> fields; };

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
    bool jsonOutput = false;
    bool quiet = false;
    bool verbose = false;
    bool noColor = false;
    bool trace = false;
    bool profile = false;
    bool cliModeExplicit = false;
    bool cliInjectionExplicit = false;
    bool cliStrictExplicit = false;
    bool trustScriptDirectives = false;
    int timeoutMs = 0;
    int maxSteps = 100000;
    std::string outputFormat = "text";
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
    bool timeoutEnabled = false;
    std::chrono::steady_clock::time_point timeoutDeadline;
    std::unordered_map<std::string, WapiValue> variables;
    std::unordered_set<std::string> immutableBindings;
    std::unordered_map<std::string, std::shared_ptr<FunctionDeclaration>> userFunctions;
    std::unordered_map<std::string, std::shared_ptr<StructDeclaration>> structRegistry;
    std::unordered_set<long long> trackedHandles;
    std::unordered_map<long long, std::unordered_set<long long>> trackedAllocations;

    WapiValue evalNode(std::shared_ptr<ASTNode> node);
    WapiValue evalFunctionCall(std::shared_ptr<FunctionCall> call);
    WapiValue evalUnaryExpression(const UnaryExpression& expr);
    WapiValue evalBinaryExpression(const BinaryExpression& expr);
    WapiValue evalIndexExpression(const IndexExpression& expr);
    WapiValue evalMethodCall(const MethodCallExpression& expr);
    WapiValue evalNullSafeCall(const NullSafeCallExpression& expr);
    WapiValue evalFieldAccess(const FieldAccessExpression& expr);
    WapiValue evalFieldAssignment(const FieldAssignment& stmt);
    WapiValue evalMatchStatement(const MatchStatement& stmt);
    WapiValue evalStructLiteral(const StructLiteral& literal);
    WapiValue evalUserFunction(const std::shared_ptr<FunctionDeclaration>& declaration, const std::shared_ptr<FunctionCall>& call);
    WapiValue evalBuiltInFunction(const std::shared_ptr<FunctionCall>& call);

    void checkArgCount(const std::shared_ptr<FunctionCall>& call, size_t expected);
    void enforceTimeout() const;
    std::string modeToString() const;
    std::string nowIso8601() const;
    std::string valueToString(const WapiValue& value) const;
    bool typeMatches(const std::string& typeName, const WapiValue& value) const;
    WapiArrayPtr asArrayValue(const WapiValue& value, const std::string& context) const;
    bool isTruthy(const WapiValue& value) const;
    long long asNumberValue(const WapiValue& value, const std::string& context) const;
    void emitJsonEvent(const std::string& kind, const std::string& payload) const;
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
    WapiValue wapi_protectMemory(long long handle, long long address, int size, int protection);
    WapiValue wapi_queryMemory(long long handle, long long address);


    WapiValue wapi_listModules(int pid);
	WapiValue wapi_getModuleBaseAddress(int pid, const std::string& moduleName);
    WapiValue wapi_getModuleSize(int pid, const std::string& moduleName);



    WapiValue wapi_closeHandle(long long handle);



    WapiValue wapi_findWindow(const std::string& name);
    WapiValue wapi_listThreads(int pid);
    WapiValue wapi_openThread(int tid);
    WapiValue wapi_suspendThread(long long threadHandle);
    WapiValue wapi_resumeThread(long long threadHandle);
    WapiValue wapi_getThreadContext(long long threadHandle);
    WapiValue wapi_setThreadContext(long long threadHandle, long long contextAddress);
    WapiValue wapi_listWindowsByPID(int pid);
    WapiValue wapi_findWindowByPID(int pid, const std::string& title);
    WapiValue wapi_sendWindowMessage(long long hwndValue, int message, long long wparam, long long lparam);
    WapiValue wapi_debugAttach(int pid);
    WapiValue wapi_debugWaitEvent();
    WapiValue wapi_debugReadRegisters(int tid);
    WapiValue wapi_debugContinue(int eventCode);
    WapiValue wapi_openProcessToken(long long handle);
    WapiValue wapi_enablePrivilege(const std::string& privilegeName);
    WapiValue wapi_sleep(long long milliseconds);



    WapiValue wapi_injectShellcode(int pid, const std::string& hexBytes);
    WapiValue wapi_createRemoteThread(int pid, long long startAddress, long long parameter);
    WapiValue wapi_injectDLL(int pid, const std::string& dllPath);
    WapiValue wapi_testInjectDLL(int pid);
};
