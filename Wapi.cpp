/*
__        ___    ____ ___
\ \      / / \  |  _ \_ _|
 \ \ /\ / / _ \ | |_) | |
  \ V  V / ___ \|  __/| |
   \_/\_/_/   \_\_|  |___|
*/



#include <iostream>
#include "lexer.h"
#include "parser.h"
#include "evaluator.h"
#include <Windows.h>
#include <shellapi.h>

void run(const std::string& source) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parse();

    Evaluator evaluator;
    evaluator.run(program);
}

int passed = 0;
int failed = 0;

void test(const std::string& name, const std::string& script) {
    // suppress output during test
    std::streambuf* old = std::cout.rdbuf(nullptr);

    try {
        run(script);
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

int main() {

    try {
        run(R"(
        int pid = findProcessPID("notepad")
        testInjectDLL(pid)
    )");
    }
    catch (const std::exception& e) {
        std::cout << "Injection test: " << e.what() << "\n";
    }

	std::cout << "\nTo run functionality tests,\n";

    system("pause");

    // Run functionality tests

    std::cout << "--- Wapi Coverage Test ---\n\n";

    // Process
    std::cout << "[Process] ---------------------------------------------\n";
    test("listProcesses", "listProcesses()");
    test("findProcessPID", "int pid = findProcessPID(\"notepad\")");
    test("openProcess", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid))");
    test("suspendProcess", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) suspendProcess(handle))");
    test("resumeProcess", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) resumeProcess(handle))");

    // Memory
    std::cout << "\n[Memory] /UNSTABLE\\ ---------------------------------\n";
    test("readMemory", R"(int pid = findProcessPID("target") int handle = openProcess(pid) int val = readMemory(handle, 140698252431448))");
    test("writeMemory", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) writeMemory(handle, 0x1000, 42))");
    test("allocMemory", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) int addr = allocMemory(handle, 1024))");
    test("freeMemory", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) int addr = allocMemory(handle, 1024) freeMemory(handle, addr))");

    // Window
    std::cout << "\n[Window] --------------------------------------------\n";
    test("findWindow", "int win = findWindow(\"Notepad\")");
    test("getWindowTitle", "int pid = findProcessPID(\"notepad\") int title = getWindowTitle(pid)");
    test("sendMessage", "int win = findWindow(\"Notepad\") sendMessage(win, 0)");
    test("showWindow", "int win = findWindow(\"Notepad\") showWindow(win)");
    test("hideWindow", "int win = findWindow(\"Notepad\") hideWindow(win)");

    // Thread
    std::cout << "\n[Thread] --------------------------------------------\n";
    test("createThread", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) int t = createThread(handle, 0))");
    test("suspendThread", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) int t = createThread(handle, 0) suspendThread(t))");
    test("resumeThread", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) int t = createThread(handle, 0) resumeThread(t))");

    // File
    std::cout << "\n[File] ----------------------------------------------\n";
    test("readFile", "int content = readFile(\"C:\\\\test.txt\")");
    test("writeFile", "writeFile(\"C:\\\\test.txt\", \"hello\")");
    test("deleteFile", "deleteFile(\"C:\\\\test.txt\")");
    test("fileExists", "int exists = fileExists(\"C:\\\\test.txt\")");

    // Registry
    std::cout << "\n[Registry] ------------------------------------------\n";
    test("readRegistry", "int val = readRegistry(\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\", \"test\")");
    test("writeRegistry", "writeRegistry(\"HKEY_LOCAL_MACHINE\\\\SOFTWARE\", \"test\", \"value\")");

    // Termination
	std::cout << "\n[Termination] ---------------------------------------\n";
    test("terminateProcess", R"(int pid = findProcessPID("notepad") int handle = openProcess(pid) terminateProcess(handle))");

    int total = passed + failed;
    float percent = (float)passed / total * 100;
    std::cout << "\n--- Results ---\n";
    std::cout << passed << "/" << total << " tests passed (" << percent << "%)\n";
    std::cout << failed << " failed\n";

	system("pause");

    return 0;
}
