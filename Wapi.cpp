/*
__        ___    ____ ___
\ \      / / \  |  _ \_ _|
 \ \ /\ / / _ \ | |_) | |
  \ V  V / ___ \|  __/| |
   \_/\_/_/   \_\_|  |___| v0.01
*/

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "evaluator.h"
#include "lexer.h"
#include "parser.h"

namespace {

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  wapi run \"<script>\" [--mode safe|dev|unsafe] [--allow-injection] [--strict-permissions] [--cap <name>]...\n"
        << "  wapi check \"<script>\" [--mode safe|dev|unsafe] [--allow-injection] [--strict-permissions] [--cap <name>]...\n"
        << "  wapi test\n"
        << "\n"
        << "Examples:\n"
        << "  wapi run \"int pid = findProcessPID(\\\"notepad\\\")\" --mode safe\n"
        << "  wapi check \"int pid = findProcessPID(\\\"notepad\\\") testInjectDLL(pid)\" --allow-injection\n"
        << "  wapi test\n";
}

WapiMode parseMode(const std::string& value) {
    if (value == "safe") return WapiMode::Safe;
    if (value == "dev") return WapiMode::Dev;
    if (value == "unsafe") return WapiMode::Unsafe;
    throw std::runtime_error("Invalid mode: " + value + " (expected safe|dev|unsafe)");
}

WapiRuntimeOptions parseOptions(const std::vector<std::string>& args, bool checkOnly) {
    WapiRuntimeOptions options;
    options.checkOnly = checkOnly;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mode") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --mode");
            options.mode = parseMode(args[++i]);
            continue;
        }
        if (args[i] == "--allow-injection") {
            options.allowInjection = true;
            continue;
        }
        if (args[i] == "--strict-permissions") {
            options.strictPermissions = true;
            continue;
        }
        if (args[i] == "--cap") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --cap");
            options.capabilities.insert(args[++i]);
            continue;
        }
        throw std::runtime_error("Unknown option: " + args[i]);
    }

    return options;
}

void runScript(const std::string& source, const WapiRuntimeOptions& options) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parse();

    Evaluator evaluator(options);
    evaluator.run(program);
}

int passed = 0;
int failed = 0;

void test(const std::string& name, const std::string& script, const WapiRuntimeOptions& options) {
    std::streambuf* old = std::cout.rdbuf(nullptr);

    try {
        runScript(script, options);
        std::cout.rdbuf(old);
        std::cout << "[PASS] " << name << "\n";
        passed++;
    }
    catch (const std::exception& e) {
        std::cout.rdbuf(old);
        std::cout << "[FAIL] " << name << " - " << e.what() << "\n";
        failed++;
    }
}

void runStandardizedTests() {
    passed = 0;
    failed = 0;

    WapiRuntimeOptions options;
    options.mode = WapiMode::Safe;
    options.checkOnly = true;
    options.allowInjection = true;

    std::cout << "--- Wapi Standardized API Test Suite ---\n\n";
    std::cout << "[Precheck] Start Notepad before running tests for process-dependent coverage.\n\n";

    std::cout << "[Implemented Baseline]\n";
    std::cout << "[Process] ---------------------------------------------\n";
    test("listProcesses", "listProcesses()", options);
    test("findProcessPID", "int pid = findProcessPID(\"notepad\")", options);
    test("openProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid)", options);
    test("suspendProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) suspendProcess(handle)", options);
    test("resumeProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) resumeProcess(handle)", options);
    test("terminateProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) terminateProcess(handle)", options);

    std::cout << "\n[Memory] /UNSTABLE\\ ---------------------------------\n";
    test("readMemory", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) int val = readMemory(handle, 0x1000)", options);
    test("writeMemory", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) writeMemory(handle, 0x1000, 42)", options);
    test("allocMemory", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) int addr = allocMemory(handle, 1024)", options);
    test("freeMemory", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) int addr = allocMemory(handle, 1024) freeMemory(handle, addr)", options);

    std::cout << "\n[Window] --------------------------------------------\n";
    test("findWindow", "int win = findWindow(\"Notepad\")", options);
    test("injectDLL", "int pid = findProcessPID(\"notepad\") injectDLL(pid, \"C:\\\\Temp\\\\TestDLL.dll\")", options);
    test("testInjectDLL", "int pid = findProcessPID(\"notepad\") testInjectDLL(pid)", options);

    std::cout << "\n[Roadmap / To Implement]\n";
    std::cout << "(These are expected to fail until you implement each API)\n";
    std::cout << "[Process] ---------------------------------------------\n";
    test("closeProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) closeProcess(handle)", options);

    std::cout << "\n[Modules] --------------------------------------------\n";
    test("listModules", "int pid = findProcessPID(\"notepad\") listModules(pid)", options);
    test("getModuleBase", "int pid = findProcessPID(\"notepad\") int base = getModuleBase(pid, \"kernel32.dll\")", options);
    test("getModuleSize", "int pid = findProcessPID(\"notepad\") int size = getModuleSize(pid, \"kernel32.dll\")", options);

    std::cout << "\n[Memory Advanced] ------------------------------------\n";
    test("protectMemory", "int pid = findProcessPID(\"notepad\") int h = openProcess(pid) protectMemory(h, 0x1000, 0x1000, 0x40)", options);
    test("queryMemory", "int pid = findProcessPID(\"notepad\") int h = openProcess(pid) queryMemory(h, 0x1000)", options);

    std::cout << "\n[Threads] --------------------------------------------\n";
    test("listThreads", "int pid = findProcessPID(\"notepad\") listThreads(pid)", options);
    test("openThread", "int tid = 1 int th = openThread(tid)", options);
    test("suspendThread", "int tid = 1 int th = openThread(tid) suspendThread(th)", options);
    test("resumeThread", "int tid = 1 int th = openThread(tid) resumeThread(th)", options);
    test("getThreadContext", "int tid = 1 int th = openThread(tid) int ctx = getThreadContext(th)", options);
    test("setThreadContext", "int tid = 1 int th = openThread(tid) setThreadContext(th, 0)", options);

    std::cout << "\n[Injection Advanced] ---------------------------------\n";
    test("injectShellcode", "int pid = findProcessPID(\"notepad\") injectShellcode(pid, \"90 90 C3\")", options);
    test("createRemoteThread", "int pid = findProcessPID(\"notepad\") createRemoteThread(pid, 0x1000, 0)", options);

    std::cout << "\n[Window By PID] --------------------------------------\n";
    test("listWindowsByPID", "int pid = findProcessPID(\"notepad\") listWindowsByPID(pid)", options);
    test("findWindowByPID", "int pid = findProcessPID(\"notepad\") int w = findWindowByPID(pid, \"Notepad\")", options);
    test("sendWindowMessage", "int pid = findProcessPID(\"notepad\") int w = findWindowByPID(pid, \"Notepad\") sendWindowMessage(w, 0x000C, 0, 0)", options);

    std::cout << "\n[Debug] ----------------------------------------------\n";
    test("debugAttach", "int pid = findProcessPID(\"notepad\") debugAttach(pid)", options);
    test("debugWaitEvent", "int ev = debugWaitEvent()", options);
    test("debugReadRegisters", "int tid = 1 int r = debugReadRegisters(tid)", options);
    test("debugContinue", "debugContinue(0)", options);

    std::cout << "\n[Token / Privilege] ----------------------------------\n";
    test("openProcessToken", "int pid = findProcessPID(\"notepad\") int h = openProcess(pid) int tok = openProcessToken(h)", options);
    test("enablePrivilege", "enablePrivilege(\"SeDebugPrivilege\")", options);

    int total = passed + failed;
    float percent = total == 0 ? 0.0f : (float)passed / total * 100;
    std::cout << "\n--- Results ---\n";
    std::cout << passed << "/" << total << " tests passed (" << percent << "%)\n";
    std::cout << failed << " failed\n";

}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            printUsage();
            return 1;
        }

        std::string command = argv[1];

        if (command == "test") {
            runStandardizedTests();
            return 0;
        }

        if (command != "run" && command != "check") {
            printUsage();
            return 1;
        }

        if (argc < 3) {
            throw std::runtime_error("Missing script source argument");
        }

        std::string source = argv[2];

        std::vector<std::string> optionArgs;
        for (int i = 3; i < argc; ++i) {
            optionArgs.emplace_back(argv[i]);
        }

        WapiRuntimeOptions options = parseOptions(optionArgs, command == "check");
        runScript(source, options);

        if (options.checkOnly) {
            std::cout << "[WAPI_CHECK] Preflight completed without execution side effects\n";
        }

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Wapi error: " << e.what() << "\n";
        return 1;
    }
}
