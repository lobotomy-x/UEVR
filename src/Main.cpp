// dllmain
#include <windows.h>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <utility/Thread.hpp>

#include "Framework.hpp"

void startup_thread(HMODULE poc_module) {
// 100% of unreal games have an UnrealWindow
    static auto attempts = 5;
    HWND ue_window{nullptr};
    auto ue_window_scan = [&ue_window, &poc_module]() {
        ue_window = FindWindowA("UnrealWindow", nullptr);
        DWORD process_id = 0;
        if (ue_window != nullptr) {
            GetWindowThreadProcessId(ue_window, &process_id);
            if (process_id == GetCurrentProcessId()) return;
        } else return;
    };
    while (ue_window == nullptr && attempts > 0) {
        ue_window_scan();
        if (ue_window != nullptr) break;
        Sleep(1000);
        attempts--;
    }
    if (ue_window == nullptr) {
        FreeLibraryAndExitThread(poc_module, 0);
            return;
    }

    g_framework = std::make_unique<Framework>(poc_module);
}

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)startup_thread, handle, 0, nullptr);
    }

    return TRUE;
}