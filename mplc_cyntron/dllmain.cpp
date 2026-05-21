#include <Windows.h>

#pragma comment(lib, "opcua.lib")
#pragma comment(lib, "mplcshare.lib")
#pragma comment(lib, "masterplc.lib")
#pragma comment(lib, "lua.lib")
#pragma comment(lib, "common_drivers.lib")

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
