#include <windows.h>

#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/Patch.hpp>
#include <utility/String.hpp>
#include <utility/Thread.hpp>
#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <sdk/UObjectArray.hpp>

#define SPAWN_CONSOLE

void startup_thread() {
    // Spawn a debug console
#ifdef SPAWN_CONSOLE
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
#endif
    const auto game = utility::get_executable();
 
  
    // Set up spdlog to sink to the console
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] [pakscan] %v");
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    spdlog::set_default_logger(spdlog::stdout_logger_mt("console"));

 auto candidates = {
        L"IsNonPakFilenameAllowed",
        L"HandleUnmountPakDelegate",
        L"RegisterEncryptionKey",
        L"GetPakSigningFailureHandlerData",
        L"FileExists",
        L"GetPakSigningKeys",
        L"GetPakFolders",
        L"PakOpenRead",
        L"PakOpenAsyncRead",
        L"GetPakSigningKeysDelegate",
        L"FileIoStoreOpenContainer",
        L"GetPakFoldersGMalloc",
        L"GetPakOrder",
        L"AddPluginSearchPath",
        L"MountNewlyCreatedPlugin",
        L"StaticConstructObject_Internal",
        L"ExecMountPak",
        L"bEnablePakSigning",
        L"UnMountPak",
        L"MountPak",
        L"PakInternalInfo",
        L"bEnablePakUAssetEncryption",
        L"AdditionalFileToPak",
        L"GetPakSigningKeysDelegate"
    };
       
    for (const auto& candidate : candidates) {
        try {
        
        auto fn = utility::find_function_from_string_ref(game, candidate);
        if (fn) {
            fn = utility::find_function_start_with_call(*fn);
        }
        const auto str_data = utility::scan_string(game,  utility::narrow(candidate), false);
        if (str_data) {
            const auto str_ref = utility::scan_displacement_reference(game, *str_data);
          
        }} catch (...) {
        }
    }

    spdlog::set_default_logger(spdlog::stdout_logger_mt("console"));

    SPDLOG_INFO("Test!");

    sdk::FName::get_constructor();
    sdk::FName::get_to_string();
    sdk::FUObjectArray::get();
    SPDLOG_INFO("Test finished!");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason_for_call, LPVOID reserved) {
    switch (reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        //CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)startup_thread, nullptr, 0, nullptr);
        startup_thread();
        break;
    }

    return TRUE;
}