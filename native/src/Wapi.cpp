/*
██╗    ██╗ █████╗ ██████╗ ██╗    ██╗   ██╗ ██████╗     ██████╗ ██████╗
██║    ██║██╔══██╗██╔══██╗██║    ██║   ██║██╔═████╗   ██╔═████╗╚════██╗
██║ █╗ ██║███████║██████╔╝██║    ██║   ██║██║██╔██║   ██║██╔██║ █████╔╝
██║███╗██║██╔══██║██╔═══╝ ██║    ╚██╗ ██╔╝████╔╝██║   ████╔╝██║██╔═══╝
╚███╔███╔╝██║  ██║██║     ██║     ╚████╔╝ ╚██████╔╝██╗╚██████╔╝███████╗
 ╚══╝╚══╝ ╚═╝  ╚═╝╚═╝     ╚═╝      ╚═══╝   ╚═════╝ ╚═╝ ╚═════╝ ╚══════╝
*/

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
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
        << "  wapi run <script-or-file.wapi|-> [options]\n"
        << "  wapi check <script-or-file.wapi|-> [options]\n"
        << "  wapi repl [options]\n"
        << "  wapi fmt <script-or-file.wapi> [--write]\n"
        << "  wapi init [dir]\n"
        << "  wapi lint|validate <script-or-file.wapi|-> [options]\n"
        << "  wapi bundle <file.wapi>... [-o output.wapi]\n"
        << "  wapi completions [powershell|bash]\n"
        << "  wapi doc [function]\n"
        << "  wapi audit-report\n"
        << "  wapi errors\n"
        << "  wapi test\n"
        << "  wapi --version\n"
        << "\n"
        << "Options:\n"
        << "  --mode safe|dev|unsafe   Runtime safety mode; WAPI_MODE is used when omitted.\n"
        << "  --cap <name>             Add a capability grant.\n"
        << "  --allow-injection        Permit injection helpers outside unsafe mode.\n"
        << "  --strict-permissions     Turn missing capabilities into errors.\n"
        << "  --json|--output json     Emit JSON runtime events.\n"
        << "  --output csv             Accepted for tooling compatibility.\n"
        << "  --quiet                  Suppress audit/warning chatter.\n"
        << "  --verbose                Enable verbose CLI status.\n"
        << "  --trace                  Print a CLI trace line before execution.\n"
        << "  --profile                Print elapsed execution time.\n"
        << "  --timeout <ms>           Parsed for wrappers; native calls are not interrupted yet.\n"
        << "  --max-steps <n>          Override loop guard iteration limit.\n"
        << "  --env KEY=VALUE          Set an environment variable for this run.\n"
        << "  --dry-run                Alias for check/preflight mode.\n"
        << "  --watch                  Accepted by run/check for editor integrations.\n"
        << "\n"
        << "Examples:\n"
        << "  wapi run \"int pid = findProcessPID(\\\"notepad\\\")\" --mode safe\n"
        << "  wapi check script.wapi --quiet --max-steps 1000\n"
        << "  type script.wapi | wapi run -\n";
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

    char* envMode = nullptr;
    size_t envModeSize = 0;
    if (_dupenv_s(&envMode, &envModeSize, "WAPI_MODE") == 0 && envMode != nullptr) {
        if (*envMode) options.mode = parseMode(envMode);
        free(envMode);
    }

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
        if (args[i] == "--json") {
            options.jsonOutput = true;
            options.outputFormat = "json";
            continue;
        }
        if (args[i] == "--output") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --output");
            std::string value = args[++i];
            if (value != "json" && value != "csv" && value != "text") throw std::runtime_error("Invalid --output value: " + value + " (expected text|json|csv)");
            options.outputFormat = value;
            options.jsonOutput = value == "json";
            continue;
        }
        if (args[i] == "--cap") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --cap");
            options.capabilities.insert(args[++i]);
            continue;
        }
        if (args[i] == "--quiet") {
            options.quiet = true;
            continue;
        }
        if (args[i] == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (args[i] == "--no-color") {
            options.noColor = true;
            continue;
        }
        if (args[i] == "--trace") {
            options.trace = true;
            continue;
        }
        if (args[i] == "--profile") {
            options.profile = true;
            continue;
        }
        if (args[i] == "--timeout") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --timeout");
            options.timeoutMs = std::stoi(args[++i]);
            continue;
        }
        if (args[i] == "--max-steps") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --max-steps");
            options.maxSteps = (std::max)(1, std::stoi(args[++i]));
            continue;
        }
        if (args[i] == "--env") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --env");
            const std::string pair = args[++i];
            const size_t equals = pair.find('=');
            if (equals == std::string::npos) throw std::runtime_error("Invalid --env value, expected KEY=VALUE");
            _putenv_s(pair.substr(0, equals).c_str(), pair.substr(equals + 1).c_str());
            continue;
        }
        if (args[i] == "--dry-run") {
            options.checkOnly = true;
            continue;
        }
        if (args[i] == "--watch") {
            continue;
        }
        throw std::runtime_error("Unknown option: " + args[i]);
    }

    return options;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not read file: " + path.string());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string readStdin() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return buffer.str();
}

void writeTextFileIfMissing(const std::filesystem::path& path, const std::string& content) {
    if (std::filesystem::exists(path)) return;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Could not write file: " + path.string());
    file << content;
}

void runInit(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir / "src");
    writeTextFileIfMissing(dir / "wapi.toml", "mode = \"safe\"\nentry = \"src/main.wapi\"\n");
    writeTextFileIfMissing(dir / "src" / "main.wapi", "print(\"hello from wapi\")\n");
    std::cout << "Initialized Wapi project at " << std::filesystem::weakly_canonical(dir).string() << "\n";
}

void printErrors() {
    std::cout
        << "E_LEX             Lexer error\n"
        << "E_PARSE           Parser error\n"
        << "E_TYPE            Type mismatch\n"
        << "E_ARG_COUNT       Wrong argument count\n"
        << "E_ARG_TYPE        Wrong argument type\n"
        << "E_UNDEFINED_VAR   Missing variable\n"
        << "E_CONST_ASSIGN    Assignment to const variable\n"
        << "E_DIVIDE_BY_ZERO  Divide or modulo by zero\n"
        << "E_INDEX_OUT_OF_RANGE Index outside string/array\n"
        << "E_LOOP_LIMIT      Loop guard exceeded\n"
        << "E_PERMISSION      Runtime capability denied\n"
        << "E_ASSERT          assert(...) failed\n";
}

void printCompletions(const std::string& shell) {
    if (shell == "bash") {
        std::cout << "complete -W 'run check repl fmt help test init lint validate bundle completions doc audit-report errors --version' wapi\n";
        return;
    }
    std::cout << "Register-ArgumentCompleter -Native -CommandName wapi -ScriptBlock { param($wordToComplete) 'run','check','repl','fmt','help','test','init','lint','validate','bundle','completions','doc','audit-report','errors','--version' | Where-Object { $_ -like \"$wordToComplete*\" } }\n";
}

void printAuditReportTemplate() {
    std::cout
        << "{\n"
        << "  \"tool\": \"wapi\",\n"
        << "  \"mode\": \"safe\",\n"
        << "  \"events\": []\n"
        << "}\n";
}

void printUpdateInfo() {
    std::cout
        << "Wapi does not have a self-updater yet. Build from source or install a packaged release when one is published.\n"
        << "Winget packaging is not configured in this repo yet.\n";
}
std::string trimCopy(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string expandIncludes(const std::string& source, const std::filesystem::path& baseDir, std::unordered_set<std::string>& seen) {
    std::ostringstream out;
    std::istringstream input(source);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trimCopy(line);
        if (trimmed.rfind("include \"", 0) == 0 && trimmed.size() > 10 && trimmed.back() == '"') {
            std::filesystem::path includePath = baseDir / trimmed.substr(9, trimmed.size() - 10);
            includePath = std::filesystem::weakly_canonical(includePath);
            const std::string key = includePath.string();
            if (!seen.insert(key).second) throw std::runtime_error("E_INCLUDE_CYCLE: " + key);
            out << expandIncludes(readTextFile(includePath), includePath.parent_path(), seen) << "\n";
            continue;
        }
        out << line << "\n";
    }
    return out.str();
}

std::string resolveSourceArgument(const std::string& argument) {
    if (argument == "-") return readStdin();
    std::filesystem::path path(argument);
    std::unordered_set<std::string> seen;
    if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
        path = std::filesystem::weakly_canonical(path);
        seen.insert(path.string());
        return expandIncludes(readTextFile(path), path.parent_path(), seen);
    }
    return expandIncludes(argument, std::filesystem::current_path(), seen);
}

std::string resolveSourceArguments(const std::vector<std::string>& arguments) {
    std::ostringstream source;
    for (const auto& argument : arguments) source << resolveSourceArgument(argument) << "\n";
    return source.str();
}

std::string formatSource(const std::string& source) {
    std::ostringstream out;
    std::istringstream input(source);
    std::string line;
    int depth = 0;
    while (std::getline(input, line)) {
        std::string trimmed = trimCopy(line);
        if (trimmed.empty()) { out << "\n"; continue; }
        if (!trimmed.empty() && trimmed[0] == '}') depth = (std::max)(0, depth - 1);
        out << std::string(static_cast<size_t>(depth * 4), ' ') << trimmed << "\n";
        for (char ch : trimmed) {
            if (ch == '{') depth++;
            if (ch == '}') depth = (std::max)(0, depth - 1);
        }
    }
    return out.str();
}

void printFunctionHelp(const std::string& name) {
    struct Help { const char* name; const char* signature; const char* note; };
    static const std::vector<Help> help = {
        {"len", "len(value)", "Length of a string or array."},
        {"substr", "substr(text, start, count)", "Slice part of a string."},
        {"contains", "contains(text, needle)", "True when text contains needle."},
        {"replace", "replace(text, needle, replacement)", "Replace all matching text."},
        {"toLower", "toLower(text)", "Lowercase ASCII text."},
        {"toInt", "toInt(value)", "Convert a string or number to int."},
        {"abs", "abs(number)", "Absolute numeric value."},
        {"min", "min(a, b)", "Smaller numeric value."},
        {"max", "max(a, b)", "Larger numeric value."},
        {"push", "push(array, value)", "Append to an array and return it."},
        {"pop", "pop(array)", "Remove and return the last array item."},
        {"print", "print(value)", "Write a value to stdout."}
    };
    if (name.empty()) {
        std::cout << "Available help topics:\n";
        for (const auto& item : help) std::cout << "  " << item.name << "\n";
        return;
    }
    for (const auto& item : help) {
        if (name == item.name) {
            std::cout << item.signature << "\n" << item.note << "\n";
            return;
        }
    }
    std::cout << "No built-in help for " << name << ". Runtime WinAPI calls are listed in the IDE Functions panel.\n";
}
void runScript(const std::string& source, const WapiRuntimeOptions& options) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parse();

    Evaluator evaluator(options);
    evaluator.run(program);
}

void runRepl(const WapiRuntimeOptions& options) {
    std::cout << "Wapi REPL. Type :quit to exit.\n";
    std::string line;
    while (true) {
        std::cout << "wapi> ";
        if (!std::getline(std::cin, line)) break;
        const std::string trimmed = trimCopy(line);
        if (trimmed == ":quit" || trimmed == ":exit") break;
        if (trimmed.empty()) continue;
        try {
            runScript(resolveSourceArgument(line), options);
        }
        catch (const std::exception& e) {
            std::cerr << "Wapi error: " << e.what() << "\n";
        }
    }
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
    options.mode = WapiMode::Dev;
    options.checkOnly = true;
    options.allowInjection = true;

    std::cout << "--- Wapi Standardized API Test Suite ---\n\n";
    std::cout << "[Precheck] Start Notepad before running tests for process-dependent coverage.\n\n";

    std::cout << "[Implemented Baseline]\n";
    std::cout << "[Language] --------------------------------------------\n";
    test("arithmetic expressions", "int size = 1024 * 4 int next = size + 8", options);
    test("if/else blocks", "bool ready = false int value = 1 if ready { value = 2 } else { value = value + 3 }", options);
    test("while loops", "int i = 0 while i < 3 { i = i + 1 }", options);
    test("print expression", "print(\"value=\" + 42)", options);
    test("namespaced runtime call", "proc.list()", options);

    std::cout << "[Process] ---------------------------------------------\n";
    test("listProcesses", "listProcesses()", options);
    test("findProcessPID", "int pid = findProcessPID(\"notepad\")", options);
    test("openProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid)", options);
    test("suspendProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) suspendProcess(handle)", options);
    test("resumeProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) resumeProcess(handle)", options);
    test("terminateProcess", "int pid = findProcessPID(\"notepad\") int handle = openProcess(pid) terminateProcess(handle)", options);

    std::cout << "\n[Memory] --------------------------------------------\n";
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
            return 64;
        }

        std::string command = argv[1];

        if (command == "--version" || command == "-v") {
            std::cout << "wapi 0.2\n";
            return 0;
        }

        if (command == "help" || command == "doc") {
            printFunctionHelp(argc >= 3 ? argv[2] : "");
            return 0;
        }

        if (command == "errors") {
            printErrors();
            return 0;
        }

        if (command == "completions") {
            printCompletions(argc >= 3 ? argv[2] : "powershell");
            return 0;
        }

        if (command == "audit-report") {
            printAuditReportTemplate();
            return 0;
        }

        if (command == "update" || command == "winget") {
            printUpdateInfo();
            return 0;
        }

        if (command == "init") {
            runInit(argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::current_path());
            return 0;
        }

        if (command == "test") {
            runStandardizedTests();
            return failed == 0 ? 0 : 1;
        }

        if (command == "bundle") {
            std::vector<std::string> sourceArgs;
            std::filesystem::path outputPath;
            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "-o" || arg == "--output-file") {
                    if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
                    outputPath = argv[++i];
                    continue;
                }
                sourceArgs.push_back(arg);
            }
            if (sourceArgs.empty()) throw std::runtime_error("Missing source file for bundle");
            const std::string bundled = resolveSourceArguments(sourceArgs);
            if (!outputPath.empty()) {
                std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
                if (!out) throw std::runtime_error("Could not write bundle: " + outputPath.string());
                out << bundled;
            }
            else {
                std::cout << bundled;
            }
            return 0;
        }

        if (command == "fmt") {
            if (argc < 3) throw std::runtime_error("Missing script source or file for fmt");
            const std::string target = argv[2];
            bool write = false;
            for (int i = 3; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "--write") write = true;
                else throw std::runtime_error("Unknown fmt option: " + option);
            }
            const bool isFile = std::filesystem::exists(std::filesystem::path(target));
            const std::string formatted = formatSource(isFile ? readTextFile(target) : target);
            if (write) {
                if (!isFile) throw std::runtime_error("fmt --write requires a file path");
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (!out) throw std::runtime_error("Could not write file: " + target);
                out << formatted;
            }
            else {
                std::cout << formatted;
            }
            return 0;
        }

        if (command == "repl") {
            std::vector<std::string> optionArgs;
            for (int i = 2; i < argc; ++i) optionArgs.emplace_back(argv[i]);
            WapiRuntimeOptions options = parseOptions(optionArgs, false);
            runRepl(options);
            return 0;
        }

        if (command == "lint" || command == "validate") command = "check";

        if (command != "run" && command != "check") {
            printUsage();
            return 64;
        }

        std::vector<std::string> sourceArgs;
        std::vector<std::string> optionArgs;
        bool optionMode = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (!optionMode && arg.rfind("--", 0) == 0) optionMode = true;
            if (optionMode) optionArgs.push_back(arg);
            else sourceArgs.push_back(arg);
        }

        if (sourceArgs.empty()) throw std::runtime_error("Missing script source or file argument");

        WapiRuntimeOptions options = parseOptions(optionArgs, command == "check");
        if (options.verbose && options.timeoutMs > 0 && !options.quiet) {
            std::cout << "[WAPI_INFO] timeout parsed as " << options.timeoutMs << "ms; native interruption is not enabled yet\n";
        }
        if (options.trace && !options.quiet) {
            std::cout << "[WAPI_TRACE] command=" << command << " sources=" << sourceArgs.size() << " mode=" << (options.checkOnly ? "check" : "run") << "\n";
        }

        const auto started = std::chrono::steady_clock::now();
        runScript(resolveSourceArguments(sourceArgs), options);

        if (options.checkOnly && !options.quiet) {
            std::cout << "[WAPI_CHECK] Preflight completed without execution side effects\n";
        }
        if (options.profile && !options.quiet) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            std::cout << "[WAPI_PROFILE] elapsedMs=" << elapsed << "\n";
        }

        return 0;
    }
    catch (const std::exception& e) {
        const std::string message = e.what();
        std::cerr << "Wapi error: " << message << "\n";
        if (message.rfind("E_LEX", 0) == 0 || message.rfind("E_PARSE", 0) == 0) return 65;
        if (message.rfind("E_TYPE", 0) == 0 || message.rfind("E_ARG", 0) == 0) return 66;
        if (message.rfind("E_PERMISSION", 0) == 0) return 77;
        return 1;
    }
}
