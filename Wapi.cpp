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
        << "\n"
        << "Examples:\n"
        << "  wapi run \"int pid = findProcessPID(\\\"notepad\\\")\" --mode safe\n"
        << "  wapi check \"int pid = findProcessPID(\\\"notepad\\\") testInjectDLL(pid)\" --allow-injection\n";
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

void runLegacyTests() {
    WapiRuntimeOptions options;
    options.mode = WapiMode::Safe;

    std::cout << "--- Wapi Coverage Test ---\n\n";

    std::cout << "[Process] ---------------------------------------------\n";
    test("listProcesses", "listProcesses()", options);
    test("findProcessPID", "int pid = findProcessPID(\"notepad\")", options);
    test("openProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid)", options);
    test("suspendProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) suspendProcess(handle)", options);
    test("resumeProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) resumeProcess(handle)", options);

    std::cout << "\n[Memory] /UNSTABLE\\ ---------------------------------\n";
    test("readMemory", "int pid = findProcessPID(\"target\") int handle = openProcess(pid) int val = readMemory(handle, 140698252431448)", options);
    test("writeMemory", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) writeMemory(handle, 0x1000, 42)", options);
    test("allocMemory", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) int addr = allocMemory(handle, 1024)", options);
    test("freeMemory", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) int addr = allocMemory(handle, 1024) freeMemory(handle, addr)", options);

    std::cout << "\n[Window] --------------------------------------------\n";
    test("findWindow", "int win = findWindow(\"Notepad\")", options);

    std::cout << "\n[Termination] ---------------------------------------\n";
    test("terminateProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) terminateProcess(handle)", options);

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
            runLegacyTests();
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
