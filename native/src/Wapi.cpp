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
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
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
        << "  --trust-script-directives\n"
        << "                           Grant #mode/#cap/#allow-injection from trusted scripts.\n"
        << "  --allow-injection        Permit injection helpers outside unsafe mode.\n"
        << "  --strict-permissions     Turn missing capabilities into errors.\n"
        << "  --json|--output json     Emit JSON runtime events.\n"
        << "  --output csv             Accepted for tooling compatibility.\n"
        << "  --quiet                  Suppress audit/warning chatter.\n"
        << "  --verbose                Enable verbose CLI status.\n"
        << "  --trace                  Print a CLI trace line before execution.\n"
        << "  --profile                Print elapsed execution time.\n"
        << "  --timeout <ms>           Stop script execution after this many milliseconds.\n"
        << "  --max-steps <n>          Override loop guard iteration limit.\n"
        << "  --env KEY=VALUE          Set an environment variable for this run.\n"
        << "  --dry-run                Alias for check/preflight mode.\n"
        << "  --watch                  Accepted by run/check for editor integrations.\n"
        << "\n"
        << "File directives (top of any .wapi file, before any code):\n"
        << "  #mode safe|dev|unsafe        Minimum mode this script requires.\n"
        << "  #cap <name> [<name>...]      Declared capability requirements, not grants.\n"
        << "  #include \"path/to/file.wapi\" Include a relative .wapi file inside the source root.\n"
        << "  #strict                      Treat missing capabilities as errors.\n"
        << "  #allow-injection             Declare injection helper usage.\n"
        << "  #name \"My Script\"            Script display name.\n"
        << "  #version \"1.0.0\"             Script version string.\n"
        << "  #author \"ethical-tyce\"       Script author.\n"
        << "  #description \"...\"           One-line description shown in wapi doc output.\n"
        << "\n"
        << "Syntax highlights:\n"
        << "  var/let/const inference, {expr} string interpolation, -> return types, match, structs, ?. and ??.\n"
        << "  Methods: text.len(), text.contains(value), items.push(value), items.pop().\n"
        << "\n"
        << "Examples:\n"
        << "  wapi lint script.wapi --mode safe\n"
        << "  wapi check script.wapi --mode safe --strict-permissions --cap runtime.print\n"
        << "  type script.wapi | wapi run -\n";
}

WapiMode parseMode(const std::string& value) {
    if (value == "safe") return WapiMode::Safe;
    if (value == "dev") return WapiMode::Dev;
    if (value == "unsafe") return WapiMode::Unsafe;
    throw std::runtime_error("Invalid mode: " + value + " (expected safe|dev|unsafe)");
}

std::string modeToDirectiveString(WapiMode mode) {
    if (mode == WapiMode::Dev) return "dev";
    if (mode == WapiMode::Unsafe) return "unsafe";
    return "safe";
}

struct ScriptDirectives {
    bool hasMode = false;
    WapiMode mode = WapiMode::Safe;
    std::unordered_set<std::string> capabilities;
    std::vector<std::string> includes;
    bool allowInjection = false;
    bool strict = false;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
};

struct ExtractResult {
    std::string source;
    ScriptDirectives directives;
};

struct ResolvedSource {
    std::string source;
    ScriptDirectives directives;
};

void mergeDirectives(ScriptDirectives& target, const ScriptDirectives& incoming) {
    if (incoming.hasMode) {
        target.hasMode = true;
        target.mode = incoming.mode;
    }
    target.capabilities.insert(incoming.capabilities.begin(), incoming.capabilities.end());
    target.includes.insert(target.includes.end(), incoming.includes.begin(), incoming.includes.end());
    target.allowInjection = target.allowInjection || incoming.allowInjection;
    target.strict = target.strict || incoming.strict;
    if (!incoming.name.empty()) target.name = incoming.name;
    if (!incoming.version.empty()) target.version = incoming.version;
    if (!incoming.author.empty()) target.author = incoming.author;
    if (!incoming.description.empty()) target.description = incoming.description;
}

WapiRuntimeOptions parseOptions(const std::vector<std::string>& args, bool checkOnly) {
    WapiRuntimeOptions options;
    options.checkOnly = checkOnly;

    char* envMode = nullptr;
    size_t envModeSize = 0;
    if (_dupenv_s(&envMode, &envModeSize, "WAPI_MODE") == 0 && envMode != nullptr) {
        if (*envMode) {
            options.mode = parseMode(envMode);
            options.cliModeExplicit = true;
        }
        free(envMode);
    }

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mode") {
            if (i + 1 >= args.size()) throw std::runtime_error("Missing value for --mode");
            options.mode = parseMode(args[++i]);
            options.cliModeExplicit = true;
            continue;
        }
        if (args[i] == "--allow-injection") {
            options.allowInjection = true;
            options.cliInjectionExplicit = true;
            continue;
        }
        if (args[i] == "--trust-script-directives") {
            options.trustScriptDirectives = true;
            continue;
        }
        if (args[i] == "--strict-permissions") {
            options.strictPermissions = true;
            options.cliStrictExplicit = true;
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
            if (options.timeoutMs < 0) throw std::runtime_error("Invalid --timeout value: expected >= 0");
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

std::string stripUtf8Bom(std::string value) {
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
    return value;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not read file: " + path.string());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return stripUtf8Bom(buffer.str());
}

std::string readStdin() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return stripUtf8Bom(buffer.str());
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
    writeTextFileIfMissing(
        dir / "src" / "main.wapi",
        "#name \"Wapi starter\"\n"
        "#mode safe\n"
        "#strict\n"
        "#cap runtime.print\n"
        "\n"
        "print(\"hello from wapi\")\n"
    );
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

std::string lowerAsciiCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isPathInsideRoot(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (ec || relative.empty()) return false;
    for (const auto& part : relative) {
        if (part == "..") return false;
    }
    return true;
}

std::filesystem::path canonicalDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec || canonical.empty()) canonical = std::filesystem::absolute(path, ec);
    if (ec || canonical.empty()) throw std::runtime_error("E_INCLUDE_PATH: could not resolve include root");
    return canonical;
}

std::filesystem::path resolveIncludePath(
    const std::filesystem::path& baseDir,
    const std::filesystem::path& includeRoot,
    const std::string& requested
) {
    const std::string trimmed = trimCopy(requested);
    if (trimmed.empty()) throw std::runtime_error("E_INCLUDE_PATH: include path is empty");

    std::filesystem::path relativePath(trimmed);
    if (relativePath.is_absolute() || relativePath.has_root_name() || relativePath.has_root_directory()) {
        throw std::runtime_error("E_INCLUDE_PATH: absolute include paths are not allowed: " + trimmed);
    }
    if (lowerAsciiCopy(relativePath.extension().string()) != ".wapi") {
        throw std::runtime_error("E_INCLUDE_PATH: include target must be a .wapi file: " + trimmed);
    }

    const std::filesystem::path root = canonicalDirectory(includeRoot);
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(baseDir / relativePath, ec);
    if (ec || !std::filesystem::is_regular_file(resolved)) {
        throw std::runtime_error("E_INCLUDE_PATH: include file was not found: " + trimmed);
    }
    if (!isPathInsideRoot(resolved, root)) {
        throw std::runtime_error("E_INCLUDE_PATH: include escapes the source root: " + trimmed);
    }
    return resolved;
}

ExtractResult extractDirectives(const std::string& raw) {
    ScriptDirectives dir;
    std::ostringstream cleaned;
    std::istringstream in(raw);
    std::string line;

    while (std::getline(in, line)) {
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty()) {
            cleaned << '\n';
            continue;
        }
        if (trimmed[0] != '#') {
            cleaned << line << '\n';
            break;
        }

        const std::string rest = trimCopy(trimmed.substr(1));
        if (rest.rfind("mode ", 0) == 0) {
            dir.hasMode = true;
            dir.mode = parseMode(trimCopy(rest.substr(5)));
        }
        else if (rest.rfind("cap ", 0) == 0) {
            std::istringstream caps(rest.substr(4));
            std::string cap;
            while (caps >> cap) dir.capabilities.insert(cap);
        }
        else if (rest.rfind("include \"", 0) == 0 && rest.size() > 10 && rest.back() == '"') {
            dir.includes.push_back(rest.substr(9, rest.size() - 10));
        }
        else if (rest == "strict") {
            dir.strict = true;
        }
        else if (rest == "allow-injection") {
            dir.allowInjection = true;
        }
        else if (rest.rfind("name \"", 0) == 0 && rest.back() == '"') {
            dir.name = rest.substr(6, rest.size() - 7);
        }
        else if (rest.rfind("version \"", 0) == 0 && rest.back() == '"') {
            dir.version = rest.substr(9, rest.size() - 10);
        }
        else if (rest.rfind("author \"", 0) == 0 && rest.back() == '"') {
            dir.author = rest.substr(8, rest.size() - 9);
        }
        else if (rest.rfind("description \"", 0) == 0 && rest.back() == '"') {
            dir.description = rest.substr(13, rest.size() - 14);
        }
        else {
            std::cerr << "[WAPI_WARN] unknown directive: #" << rest << '\n';
            cleaned << "// #" << rest << '\n';
            continue;
        }
        cleaned << '\n';
    }

    while (std::getline(in, line)) cleaned << line << '\n';
    return { cleaned.str(), dir };
}

std::string expandIncludes(
    const std::string& source,
    const std::filesystem::path& baseDir,
    const std::filesystem::path& includeRoot,
    std::unordered_set<std::string>& seen,
    ScriptDirectives& directives
) {
    std::ostringstream out;
    std::istringstream input(source);
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trimCopy(line);
        const bool isOldStyle = trimmed.rfind("include \"", 0) == 0 && trimmed.size() > 10 && trimmed.back() == '"';
        const bool isNewStyle = trimmed.rfind("#include \"", 0) == 0 && trimmed.size() > 11 && trimmed.back() == '"';
        if (isOldStyle || isNewStyle) {
            const size_t skip = isOldStyle ? 9 : 10;
            const std::filesystem::path includePath = resolveIncludePath(baseDir, includeRoot, trimmed.substr(skip, trimmed.size() - skip - 1));
            const std::string key = includePath.string();
            if (!seen.insert(key).second) throw std::runtime_error("E_INCLUDE_CYCLE: " + key);
            const ExtractResult nested = extractDirectives(readTextFile(includePath));
            mergeDirectives(directives, nested.directives);
            out << expandIncludes(nested.source, includePath.parent_path(), includeRoot, seen, directives) << "\n";
            continue;
        }
        out << line << "\n";
    }
    return out.str();
}

ResolvedSource resolveSourceWithDirectives(const std::string& argument, const std::filesystem::path& cwd) {
    std::string raw;
    std::filesystem::path baseDir = cwd;
    std::unordered_set<std::string> seen;

    if (argument == "-") {
        raw = readStdin();
    }
    else {
        std::filesystem::path path(argument);
        if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
            path = std::filesystem::weakly_canonical(path);
            baseDir = path.parent_path();
            seen.insert(path.string());
            raw = readTextFile(path);
        }
        else {
            raw = argument;
        }
    }

    ExtractResult extracted = extractDirectives(raw);
    ScriptDirectives directives = extracted.directives;
    std::ostringstream withIncludes;
    const std::filesystem::path includeRoot = canonicalDirectory(baseDir);
    for (const auto& inc : directives.includes) {
        std::filesystem::path incPath = resolveIncludePath(baseDir, includeRoot, inc);
        const std::string key = incPath.string();
        if (!seen.insert(key).second) throw std::runtime_error("E_INCLUDE_CYCLE: " + key);
        ExtractResult nested = extractDirectives(readTextFile(incPath));
        mergeDirectives(directives, nested.directives);
        withIncludes << expandIncludes(nested.source, incPath.parent_path(), includeRoot, seen, directives) << '\n';
    }
    withIncludes << expandIncludes(extracted.source, baseDir, includeRoot, seen, directives);
    return { withIncludes.str(), directives };
}

std::string resolveSourceArgument(const std::string& argument) {
    return resolveSourceWithDirectives(argument, std::filesystem::current_path()).source;
}

ResolvedSource resolveSourceArgumentsWithDirectives(const std::vector<std::string>& arguments) {
    std::ostringstream source;
    ScriptDirectives directives;
    for (const auto& argument : arguments) {
        ResolvedSource resolved = resolveSourceWithDirectives(argument, std::filesystem::current_path());
        source << resolved.source << "\n";
        mergeDirectives(directives, resolved.directives);
    }
    return { source.str(), directives };
}

std::string resolveSourceArguments(const std::vector<std::string>& arguments) {
    return resolveSourceArgumentsWithDirectives(arguments).source;
}

std::string formatSource(const std::string& source) {
    std::ostringstream out;
    std::istringstream input(source);
    std::string line;
    int depth = 0;
    bool inDirectiveBlock = true;
    while (std::getline(input, line)) {
        std::string trimmed = trimCopy(line);
        if (inDirectiveBlock) {
            if (trimmed.empty() || trimmed[0] == '#') {
                out << line << "\n";
                continue;
            }
            inDirectiveBlock = false;
        }
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
        {"print", "print(value)", "Write a value to stdout."},
        {"syntax", "var x = 1_000; let y = \"x={x}\"; match x { _ => print(y) }", "Language syntax overview."},
        {"methods", "text.len(), items.push(value), maybe?.len() ?? 0", "Method call and null-safe syntax."},
        {"struct", "struct Point { int x } Point p = Point { x: 1 }", "Declare and instantiate structured values."},
        {"directives", "#mode dev\n#cap proc.list runtime.print\n#include \"helpers.wapi\"\n#strict", "File-level directives must appear before code. Includes must be relative .wapi files inside the source root."}
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

const std::unordered_map<std::string, std::string>& functionCapabilities() {
    static const std::unordered_map<std::string, std::string> caps = {
        {"print", "runtime.print"}, {"findProcessPID", "proc.list"}, {"listProcesses", "proc.list"}, {"openProcess", "proc.open.all_access"},
        {"terminateProcess", "proc.terminate"}, {"suspendProcess", "proc.suspend"}, {"resumeProcess", "proc.resume"}, {"closeProcess", "proc.close"},
        {"readMemory", "mem.read"}, {"writeMemory", "mem.write"}, {"allocMemory", "mem.alloc"}, {"freeMemory", "mem.free"},
        {"listModules", "proc.modules"}, {"getModuleBase", "proc.modules"}, {"getModuleBaseAddress", "proc.modules"}, {"getModuleSize", "proc.modules"},
        {"protectMemory", "mem.protect"}, {"queryMemory", "mem.query"}, {"listThreads", "thread.list"}, {"openThread", "thread.open"},
        {"suspendThread", "thread.suspend"}, {"resumeThread", "thread.resume"}, {"getThreadContext", "debug.registers"}, {"setThreadContext", "thread.context.write"},
        {"injectShellcode", "inject.shellcode"}, {"createRemoteThread", "inject.shellcode"}, {"listWindowsByPID", "window.pid"}, {"findWindowByPID", "window.pid"},
        {"sendWindowMessage", "window.message"}, {"debugAttach", "debug.attach"}, {"debugWaitEvent", "debug.attach"}, {"debugReadRegisters", "debug.registers"},
        {"debugContinue", "debug.attach"}, {"openProcessToken", "token.open"}, {"enablePrivilege", "token.privilege"}, {"closeHandle", "proc.handle.close"},
        {"findWindow", "window.find"}, {"injectDLL", "inject.dll"}, {"testInjectDLL", "inject.dll"},
        {"proc.find", "proc.list"}, {"proc.list", "proc.list"}, {"proc.open", "proc.open.all_access"}, {"proc.terminate", "proc.terminate"},
        {"proc.suspend", "proc.suspend"}, {"proc.resume", "proc.resume"}, {"proc.close", "proc.close"}, {"proc.modules", "proc.modules"},
        {"proc.moduleBase", "proc.modules"}, {"proc.module.base", "proc.modules"}, {"proc.module.size", "proc.modules"},
        {"mem.read", "mem.read"}, {"mem.write", "mem.write"}, {"mem.alloc", "mem.alloc"}, {"mem.free", "mem.free"},
        {"mem.protect", "mem.protect"}, {"mem.query", "mem.query"}, {"thread.list", "thread.list"}, {"thread.open", "thread.open"},
        {"thread.suspend", "thread.suspend"}, {"thread.resume", "thread.resume"}, {"thread.context", "debug.registers"}, {"thread.context.set", "thread.context.write"},
        {"window.listByPid", "window.pid"}, {"window.findByPid", "window.pid"}, {"window.message", "window.message"},
        {"inject.shellcode", "inject.shellcode"}, {"inject.thread", "inject.shellcode"}, {"debug.attach", "debug.attach"}, {"debug.wait", "debug.attach"},
        {"debug.registers", "debug.registers"}, {"debug.continue", "debug.attach"}, {"token.open", "token.open"}, {"token.privilege", "token.privilege"},
        {"handle.close", "proc.handle.close"}, {"window.find", "window.find"}, {"inject.dll", "inject.dll"}, {"inject.testDll", "inject.dll"},
        {"inject.test", "inject.dll"}, {"testInjectDll", "inject.dll"},
        {"len", "language.core"}, {"substr", "language.string"}, {"contains", "language.string"}, {"replace", "language.string"}, {"toLower", "language.string"},
        {"toInt", "language.string"}, {"abs", "language.math"}, {"min", "language.math"}, {"max", "language.math"}, {"push", "language.array"},
        {"pop", "language.array"}, {"typeof", "language.core"}, {"assert", "language.core"}, {"toHex", "language.string"}, {"fromHex", "language.string"},
        {"split", "language.string"}, {"trim", "language.string"}, {"padLeft", "language.string"}, {"padRight", "language.string"}, {"sort", "language.array"},
        {"size", "language.core"}, {"toUpper", "language.string"}, {"startsWith", "language.string"}, {"endsWith", "language.string"},
        {"first", "language.array"}, {"last", "language.array"}
    };
    return caps;
}

void collectFunctionCalls(const std::shared_ptr<ASTNode>& node, std::unordered_set<std::string>& calls) {
    if (!node) return;
    if (auto program = std::dynamic_pointer_cast<Program>(node)) {
        for (const auto& stmt : program->statements) collectFunctionCalls(stmt, calls);
    }
    else if (auto call = std::dynamic_pointer_cast<FunctionCall>(node)) {
        calls.insert(call->name);
        for (const auto& arg : call->args) collectFunctionCalls(arg, calls);
        for (const auto& arg : call->namedArgs) collectFunctionCalls(arg.value, calls);
    }
    else if (auto method = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
        calls.insert(method->method);
        collectFunctionCalls(method->target, calls);
        for (const auto& arg : method->args) collectFunctionCalls(arg, calls);
        for (const auto& arg : method->namedArgs) collectFunctionCalls(arg.value, calls);
    }
    else if (auto ns = std::dynamic_pointer_cast<NullSafeCallExpression>(node)) {
        calls.insert(ns->method);
        collectFunctionCalls(ns->target, calls);
        for (const auto& arg : ns->args) collectFunctionCalls(arg, calls);
        for (const auto& arg : ns->namedArgs) collectFunctionCalls(arg.value, calls);
    }
    else if (auto unary = std::dynamic_pointer_cast<UnaryExpression>(node)) collectFunctionCalls(unary->value, calls);
    else if (auto binary = std::dynamic_pointer_cast<BinaryExpression>(node)) { collectFunctionCalls(binary->left, calls); collectFunctionCalls(binary->right, calls); }
    else if (auto nullish = std::dynamic_pointer_cast<NullCoalesceExpression>(node)) { collectFunctionCalls(nullish->left, calls); collectFunctionCalls(nullish->right, calls); }
    else if (auto ternary = std::dynamic_pointer_cast<TernaryExpression>(node)) { collectFunctionCalls(ternary->condition, calls); collectFunctionCalls(ternary->whenTrue, calls); collectFunctionCalls(ternary->whenFalse, calls); }
    else if (auto decl = std::dynamic_pointer_cast<VarDeclaration>(node)) collectFunctionCalls(decl->value, calls);
    else if (auto assignment = std::dynamic_pointer_cast<Assignment>(node)) collectFunctionCalls(assignment->value, calls);
    else if (auto field = std::dynamic_pointer_cast<FieldAssignment>(node)) { collectFunctionCalls(field->target, calls); collectFunctionCalls(field->value, calls); }
    else if (auto index = std::dynamic_pointer_cast<IndexAssignment>(node)) { collectFunctionCalls(index->index, calls); collectFunctionCalls(index->value, calls); }
    else if (auto block = std::dynamic_pointer_cast<BlockStatement>(node)) { for (const auto& stmt : block->statements) collectFunctionCalls(stmt, calls); }
    else if (auto fn = std::dynamic_pointer_cast<FunctionDeclaration>(node)) collectFunctionCalls(fn->body, calls);
    else if (auto ifs = std::dynamic_pointer_cast<IfStatement>(node)) { collectFunctionCalls(ifs->condition, calls); collectFunctionCalls(ifs->thenBranch, calls); collectFunctionCalls(ifs->elseBranch, calls); }
    else if (auto wh = std::dynamic_pointer_cast<WhileStatement>(node)) { collectFunctionCalls(wh->condition, calls); collectFunctionCalls(wh->body, calls); }
    else if (auto fr = std::dynamic_pointer_cast<ForRangeStatement>(node)) { collectFunctionCalls(fr->start, calls); collectFunctionCalls(fr->end, calls); collectFunctionCalls(fr->step, calls); collectFunctionCalls(fr->body, calls); }
    else if (auto ret = std::dynamic_pointer_cast<ReturnStatement>(node)) collectFunctionCalls(ret->value, calls);
    else if (auto tc = std::dynamic_pointer_cast<TryCatchStatement>(node)) { collectFunctionCalls(tc->tryBlock, calls); collectFunctionCalls(tc->catchBlock, calls); }
    else if (auto match = std::dynamic_pointer_cast<MatchStatement>(node)) {
        collectFunctionCalls(match->subject, calls);
        for (const auto& arm : match->arms) { collectFunctionCalls(arm.pattern.value, calls); collectFunctionCalls(arm.guard, calls); collectFunctionCalls(arm.body, calls); }
    }
    else if (auto literal = std::dynamic_pointer_cast<StructLiteral>(node)) { for (const auto& field : literal->fields) collectFunctionCalls(field.value, calls); }
    else if (auto access = std::dynamic_pointer_cast<FieldAccessExpression>(node)) collectFunctionCalls(access->target, calls);
    else if (auto array = std::dynamic_pointer_cast<ArrayLiteral>(node)) { for (const auto& item : array->items) collectFunctionCalls(item, calls); }
    else if (auto index = std::dynamic_pointer_cast<IndexExpression>(node)) { collectFunctionCalls(index->target, calls); collectFunctionCalls(index->index, calls); }
}

void emitLintWarnings(const std::shared_ptr<Program>& program, const ScriptDirectives& directives) {
    std::unordered_set<std::string> calls;
    std::unordered_set<std::string> usedCapabilities;
    collectFunctionCalls(program, calls);
    const auto& caps = functionCapabilities();
    for (const auto& call : calls) {
        auto found = caps.find(call);
        if (found == caps.end()) continue;
        usedCapabilities.insert(found->second);
        if (directives.capabilities.count(found->second) == 0) {
            std::cout << "[LINT] " << call << " requires capability '" << found->second << "' - add #cap " << found->second << "\n";
        }
    }
    for (const auto& cap : directives.capabilities) {
        if (usedCapabilities.count(cap) == 0) {
            std::cout << "[LINT] #cap '" << cap << "' declared but no functions using it were found\n";
        }
    }
}

void applyDirectivesToOptions(const ScriptDirectives& dir, WapiRuntimeOptions& options) {
    if (dir.hasMode) {
        if (static_cast<int>(dir.mode) > static_cast<int>(options.mode)) {
            if (options.cliModeExplicit) {
                throw std::runtime_error(
                    "E_DIRECTIVE: script requires mode '" + modeToDirectiveString(dir.mode) +
                    "' but runtime is '" + modeToDirectiveString(options.mode) +
                    "'. Re-run with --mode " + modeToDirectiveString(dir.mode)
                );
            }
            if (!options.trustScriptDirectives) {
                throw std::runtime_error(
                    "E_DIRECTIVE: script requires mode '" + modeToDirectiveString(dir.mode) +
                    "' but script directives are not trusted. Re-run with --mode " +
                    modeToDirectiveString(dir.mode) + " or --trust-script-directives"
                );
            }
            options.mode = dir.mode;
        }
        else if (!options.cliModeExplicit && options.trustScriptDirectives) {
            options.mode = dir.mode;
        }
    }
    if (options.trustScriptDirectives) {
        for (const auto& cap : dir.capabilities) options.capabilities.insert(cap);
        if (dir.allowInjection && !options.cliInjectionExplicit) options.allowInjection = true;
    }
    if (dir.strict && !options.cliStrictExplicit) options.strictPermissions = true;
    if (options.verbose && !dir.name.empty()) {
        std::cout << "[WAPI] script: " << dir.name;
        if (!dir.version.empty()) std::cout << " v" << dir.version;
        if (!dir.author.empty()) std::cout << " by " << dir.author;
        std::cout << "\n";
    }
}

std::shared_ptr<Program> parseProgram(const std::string& source) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parse();
}

void runScript(const std::string& source, const WapiRuntimeOptions& options) {
    auto program = parseProgram(source);
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

void testFailure(const std::string& name, const std::string& script, const WapiRuntimeOptions& options, const std::string& expected) {
    std::streambuf* old = std::cout.rdbuf(nullptr);

    try {
        runScript(script, options);
        std::cout.rdbuf(old);
        std::cout << "[FAIL] " << name << " - expected failure containing " << expected << "\n";
        failed++;
    }
    catch (const std::exception& e) {
        std::cout.rdbuf(old);
        const std::string message = e.what();
        if (message.find(expected) != std::string::npos) {
            std::cout << "[PASS] " << name << "\n";
            passed++;
        }
        else {
            std::cout << "[FAIL] " << name << " - " << message << "\n";
            failed++;
        }
    }
}

void testDirectiveFailure(const std::string& name, const ScriptDirectives& directives, const WapiRuntimeOptions& options, const std::string& expected) {
    try {
        WapiRuntimeOptions mutableOptions = options;
        applyDirectivesToOptions(directives, mutableOptions);
        std::cout << "[FAIL] " << name << " - expected failure containing " << expected << "\n";
        failed++;
    }
    catch (const std::exception& e) {
        const std::string message = e.what();
        if (message.find(expected) != std::string::npos) {
            std::cout << "[PASS] " << name << "\n";
            passed++;
        }
        else {
            std::cout << "[FAIL] " << name << " - " << message << "\n";
            failed++;
        }
    }
}

void runStandardizedTests() {
    passed = 0;
    failed = 0;

    WapiRuntimeOptions options;
    options.mode = WapiMode::Dev;
    options.checkOnly = true;
    options.allowInjection = true;
    for (const auto& [_, capability] : functionCapabilities()) {
        options.capabilities.insert(capability);
    }

    std::cout << "--- Wapi Standardized API Test Suite ---\n\n";
    std::cout << "[Precheck] Start Notepad before running tests for process-dependent coverage.\n\n";

    std::cout << "[Implemented Baseline]\n";
    std::cout << "[Language] --------------------------------------------\n";
    test("arithmetic expressions", "int size = 1024 * 4 int next = size + 8", options);
    test("if/else blocks", "bool ready = false int value = 1 if ready { value = 2 } else { value = value + 3 }", options);
    test("while loops", "int i = 0 while i < 3 { i = i + 1 }", options);
    test("print expression", "print(\"value=\" + 42)", options);
    test("namespaced runtime call", "proc.list()", options);
    test("syntax improvements", "var pid = 1_000 let label = \"pid={pid}\" var items = [] items.push(pid) func add(int left, int right) -> int { return left + right } struct Point { int x int y } Point p = Point { x: add(right: 2, left: 3), y: items.len() } p.x += 4 var maybe = null var fallback = maybe?.len() ?? p.x match fallback { 9 => print(label) _ => print(\"bad\") }", options);

    std::cout << "[Security Policy] -------------------------------------\n";
    WapiRuntimeOptions policyOptions;
    policyOptions.mode = WapiMode::Safe;
    policyOptions.checkOnly = true;
    test("runtime.print soft compatibility", "print(\"ok\")", policyOptions);
    testFailure("missing sensitive capability denies process listing", "proc.list()", policyOptions, "E_PERMISSION");
    policyOptions.capabilities.insert("proc.open.all_access");
    testFailure("safe mode denies all-access process handles", "proc.open(1234)", policyOptions, "requires --mode dev|unsafe");

    ScriptDirectives devDirective;
    devDirective.hasMode = true;
    devDirective.mode = WapiMode::Dev;
    testDirectiveFailure("untrusted #mode cannot raise runtime mode", devDirective, policyOptions, "script directives are not trusted");

    WapiRuntimeOptions untrustedCapOptions;
    untrustedCapOptions.mode = WapiMode::Dev;
    untrustedCapOptions.checkOnly = true;
    untrustedCapOptions.strictPermissions = true;
    ScriptDirectives privilegeDirective;
    privilegeDirective.capabilities.insert("token.privilege");
    applyDirectivesToOptions(privilegeDirective, untrustedCapOptions);
    testFailure("untrusted #cap cannot grant token privilege", "token.privilege(\"SeDebugPrivilege\")", untrustedCapOptions, "missing capability=token.privilege");

    WapiRuntimeOptions trustedDirectiveOptions;
    trustedDirectiveOptions.checkOnly = true;
    trustedDirectiveOptions.trustScriptDirectives = true;
    ScriptDirectives trustedPrivilegeDirective;
    trustedPrivilegeDirective.hasMode = true;
    trustedPrivilegeDirective.mode = WapiMode::Dev;
    trustedPrivilegeDirective.capabilities.insert("token.privilege");
    applyDirectivesToOptions(trustedPrivilegeDirective, trustedDirectiveOptions);
    test("trusted directives can grant token privilege in check mode", "token.privilege(\"SeDebugPrivilege\")", trustedDirectiveOptions);

    WapiRuntimeOptions untrustedInjectionOptions;
    untrustedInjectionOptions.mode = WapiMode::Dev;
    untrustedInjectionOptions.checkOnly = true;
    untrustedInjectionOptions.capabilities.insert("inject.shellcode");
    ScriptDirectives injectionDirective;
    injectionDirective.allowInjection = true;
    applyDirectivesToOptions(injectionDirective, untrustedInjectionOptions);
    testFailure("untrusted #allow-injection cannot grant injection flag", "inject.shellcode(1, \"90\")", untrustedInjectionOptions, "requires --allow-injection");

    WapiRuntimeOptions sleepTimeoutOptions;
    sleepTimeoutOptions.timeoutMs = 1;
    testFailure("sleep respects runtime timeout", "sleep(50)", sleepTimeoutOptions, "E_TIMEOUT");
    std::cout << "[Process] ---------------------------------------------\n";
    test("proc.list", "proc.list()", options);
    test("proc.find", "int pid = proc.find(\"notepad\")", options);
    test("proc.open", "int pid = proc.find(\"notepad\") int handle = proc.open(pid)", options);
    test("proc.suspend", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) proc.suspend(handle)", options);
    test("proc.resume", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) proc.resume(handle)", options);
    test("proc.terminate", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) proc.terminate(handle)", options);

    std::cout << "\n[Memory] --------------------------------------------\n";
    test("mem.read", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) int val = mem.read(handle, 0x1000)", options);
    test("mem.write", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) mem.write(handle, 0x1000, 42)", options);
    test("mem.alloc", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) int addr = mem.alloc(handle, 1024)", options);
    test("mem.free", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) int addr = mem.alloc(handle, 1024) mem.free(handle, addr)", options);

    std::cout << "\n[Window] --------------------------------------------\n";
    test("window.find", "int win = window.find(\"Notepad\")", options);
    test("inject.dll", "int pid = proc.find(\"notepad\") inject.dll(pid, \"C:\\\\Temp\\\\TestDLL.dll\")", options);
    test("inject.testDll", "int pid = proc.find(\"notepad\") inject.testDll(pid)", options);

    std::cout << "\n[Roadmap / To Implement]\n";
    std::cout << "(These are expected to fail until you implement each API)\n";

    std::cout << "[Process] ---------------------------------------------\n";
    test("proc.close", "int pid = proc.find(\"notepad\") int handle = proc.open(pid) proc.close(handle)", options);

    std::cout << "\n[Modules] --------------------------------------------\n";
    test("proc.modules", "int pid = proc.find(\"notepad\") proc.modules(pid)", options);
    test("proc.module.base", "int pid = proc.find(\"notepad\") int base = proc.module.base(pid, \"kernel32.dll\")", options);
    test("proc.module.size", "int pid = proc.find(\"notepad\") int size = proc.module.size(pid, \"kernel32.dll\")", options);

    std::cout << "\n[Memory Advanced] ------------------------------------\n";
    test("mem.protect", "int pid = proc.find(\"notepad\") int h = proc.open(pid) mem.protect(h, 0x1000, 0x1000, 0x40)", options);
    test("mem.query", "int pid = proc.find(\"notepad\") int h = proc.open(pid) mem.query(h, 0x1000)", options);

    std::cout << "\n[Threads] --------------------------------------------\n";
    test("thread.list", "int pid = proc.find(\"notepad\") thread.list(pid)", options);
    test("thread.open", "int tid = 1 int th = thread.open(tid)", options);
    test("thread.suspend", "int tid = 1 int th = thread.open(tid) thread.suspend(th)", options);
    test("thread.resume", "int tid = 1 int th = thread.open(tid) thread.resume(th)", options);
    test("thread.context", "int tid = 1 int th = thread.open(tid) int ctx = thread.context(th)", options);
    test("thread.context.set", "int tid = 1 int th = thread.open(tid) thread.context.set(th, 0)", options);

    std::cout << "\n[Injection Advanced] ---------------------------------\n";
    test("inject.shellcode", "int pid = proc.find(\"notepad\") inject.shellcode(pid, \"90 90 C3\")", options);
    test("inject.thread", "int pid = proc.find(\"notepad\") inject.thread(pid, 0x1000, 0)", options);

    std::cout << "\n[Window By PID] --------------------------------------\n";
    test("window.listByPid", "int pid = proc.find(\"notepad\") window.listByPid(pid)", options);
    test("window.findByPid", "int pid = proc.find(\"notepad\") int w = window.findByPid(pid, \"Notepad\")", options);
    test("window.message", "int pid = proc.find(\"notepad\") int w = window.findByPid(pid, \"Notepad\") window.message(w, 0x000C, 0, 0)", options);

    std::cout << "\n[Debug] ----------------------------------------------\n";
    test("debug.attach", "int pid = proc.find(\"notepad\") debug.attach(pid)", options);
    test("debug.wait", "int ev = debug.wait()", options);
    test("debug.registers", "int tid = 1 int r = debug.registers(tid)", options);
    test("debug.continue", "debug.continue(0)", options);

    std::cout << "\n[Token / Privilege] ----------------------------------\n";
    test("token.open", "int pid = proc.find(\"notepad\") int h = proc.open(pid) int tok = token.open(h)", options);
    test("token.privilege", "token.privilege(\"SeDebugPrivilege\")", options);

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

        const bool lintMode = command == "lint" || command == "validate";
        if (lintMode) command = "check";

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
            std::cout << "[WAPI_INFO] timeout set to " << options.timeoutMs << "ms\n";
        }
        if (options.trace && !options.quiet) {
            std::cout << "[WAPI_TRACE] command=" << command << " sources=" << sourceArgs.size() << " mode=" << (options.checkOnly ? "check" : "run") << "\n";
        }

        const auto started = std::chrono::steady_clock::now();
        ResolvedSource resolved = resolveSourceArgumentsWithDirectives(sourceArgs);
        applyDirectivesToOptions(resolved.directives, options);
        auto program = parseProgram(resolved.source);
        if (lintMode) {
            emitLintWarnings(program, resolved.directives);
            if (!options.quiet) std::cout << "[WAPI_LINT] Completed without execution\n";
            return 0;
        }
        Evaluator evaluator(options);
        evaluator.run(program);

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
        if (message.rfind("E_TIMEOUT", 0) == 0) return 124;
        return 1;
    }
}
