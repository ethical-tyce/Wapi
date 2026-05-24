#include "pch.h"
#include <Windows.h>
#include <fstream>
#include <string>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        char tempPath[MAX_PATH] = {};
        DWORD len = GetTempPathA(MAX_PATH, tempPath);
        std::string logPath = (len > 0 && len < MAX_PATH) ? std::string(tempPath) + "wapi_test.txt" : "wapi_test.txt";

        std::ofstream log(logPath);
        log << "DLL loaded!\n";
        log.close();

        MessageBoxA(NULL, "Test DLL Injection Successful!", "Wapi Injection", MB_OK | MB_SYSTEMMODAL);
    }
    return TRUE;
}
