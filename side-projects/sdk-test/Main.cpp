#include <windows.h>

#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/Patch.hpp>
#include <utility/String.hpp>
#include <utility/Thread.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <sdk/EngineModule.hpp>
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
/*


    auto is_string_nearby = [](uintptr_t addr, std::wstring_view str) {
        const auto addr_module = utility::get_module_within(addr);
        if (!addr_module) {
            return false;
        }

        const auto module_size = utility::get_module_size(*addr_module);
        const auto module_end = (uintptr_t)*addr_module + *module_size - 0x1000;

        // Find all possible strings, not just the first one
        for (auto str_addr = utility::scan_string(*addr_module, str.data(), true); str_addr.has_value();
            str_addr = utility::scan_string(*str_addr + 1, (module_end - (*str_addr + 1)), str.data(), true)) {
            // Scan for ALL references to this string
            for (auto string_ref = utility::scan_displacement_reference(*addr_module, (uintptr_t)*str_addr); string_ref.has_value();
                string_ref =
                    utility::scan_displacement_reference(*string_ref + 1, (module_end - (*string_ref + 1)), (uintptr_t)*str_addr)) {
                const auto string_ref_func_start = utility::find_function_start((uintptr_t)*string_ref);
                const auto return_addr_func_start = utility::find_function_start(addr);

                SPDLOG_INFO("String ref func start: {:x}", (uintptr_t)*string_ref_func_start);
                SPDLOG_INFO("Return addr func start: {:x}", (uintptr_t)*return_addr_func_start);

                if (string_ref_func_start && return_addr_func_start && *string_ref_func_start == *return_addr_func_start) {
                    return true;
                }
            }
        }

        return false;
    };
    auto candidates = {L"IsNonPakFilenameAllowed", L"HandleUnmountPakDelegate", L"RegisterEncryptionKey",
        L"GetPakSigningFailureHandlerData", L"FileExists", L"GetPakSigningKeys", L"GetPakFolders", L"PakOpenRead", L"PakOpenAsyncRead",
        L"GetPakSigningKeysDelegate", L"FileIoStoreOpenContainer", L"GetPakFoldersGMalloc", L"GetPakOrder", L"AddPluginSearchPath",
        L"MountNewlyCreatedPlugin", L"StaticConstructObject_Internal", L"ExecMountPak", L"bEnablePakSigning", L"UnMountPak", L"MountPak",
        L"PakInternalInfo", L"bEnablePakUAssetEncryption", L"AdditionalFileToPak", L"GetPakSigningKeysDelegate"};

    for (const auto& candidate : candidates) {
        SPDLOG_INFO(is_string_nearby(candidate))
    }*/
    spdlog::set_default_logger(spdlog::stdout_logger_mt("console"));

    SPDLOG_INFO("Test!");

                         //FCoreDelegates::GetPakSigningKeysDelegate(
    sdk::FName::get_constructor();
    sdk::FName::get_to_string();
    sdk::FUObjectArray::get();
    SPDLOG_INFO("Test finished!");


//
//0x146cc1220
//[2026-01-23 00:17:35.7991136] [PS] Found GMalloc: 0x146fd4978
//[2026-01-23 00:17:35.7993377] [PS] Found FName::ToString: 0x142707a30
//[2026-01-23 00:17:35.7994840] [PS] Found FName::FName(wchar_t*): 0x1426fb2e0
//[2026-01-23 00:17:35.7996531] [PS] Found StaticConstructObject_Internal: 0x142931f30
//[2026-01-23 00:17:35.7997774] [PS] Found FUObjectHashTables::Get(): 0x142923ba0
//[2026-01-23 00:17:35.7998549] [PS] Found GNatives: 0x147042430
//[2026-01-23 00:17:35.7999108] [PS] Failed to find ConsoleManagerSingleton: ConsoleManagerSingleton: found 2 unique values [1425CDF90, 1437D0BC0]
//[2026-01-23 00:17:35.8000348] [PS] You can supply your own AOB in 'UE4SS_Signatures/ConsoleManager.lua'
//[2026-01-23 00:17:35.8001267] [PS] Found GameEngineTick: 0x14422fd60

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