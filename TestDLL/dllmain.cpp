#include "pch.h"
#include <Windows.h>
#include <fstream>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        // write to file to confirm DLL loaded
        std::ofstream log("C:\\wapi_test.txt");
        log << "DLL loaded!\n";
        log.close();

        MessageBoxA(NULL, "Test DLL Injection Successful!", "Wapi Injection", MB_OK | MB_SYSTEMMODAL);
    }
    return TRUE;
}