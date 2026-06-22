// api_test_plugin: exercises NEW uevr/API.h features added on the luavrlib branch.
//
// Many of the new function-pointer fields in UEVR_PluginFunctions are declared
// in the public header but not yet wired into PluginLoader.cpp's
// designated-initializer (they zero-init to NULL). This plugin probes each
// pointer, logs the availability matrix, and only invokes the ones that exist.
// Calling any NULL field would crash uevr_plugin_initialize (which is exactly
// the failure mode caught on first try).
//
// Build as a Win64 DLL, drop into <Game>/UnrealVRMod/plugins/ as
// api_test_plugin.dll. Press F7 in-game to fire runtime tests; output goes to
// UEVR's log.txt with the [api_test] prefix.

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {
constexpr const char* TAG = "[api_test]";
constexpr WPARAM HOTKEY = VK_F7;

bool g_did_runtime_tests = false;
uint64_t g_tick_counter = 0;
uint64_t g_imgui_frames = 0;

#define HAS(fn) (fns->fn != nullptr)
}

class ApiTestPlugin : public Plugin {
public:
    void on_initialize() override {
        auto& api = *API::get();
        const auto fns = api.param()->functions;

        api.log_info("%s plugin loaded. branch=%s commit=%s build=%s %s", TAG,
            fns->get_branch(), fns->get_commit_hash(),
            fns->get_build_date(), fns->get_build_time());

        // Availability matrix for new fields.
        api.log_info("%s availability: load_lua_file=%d load_lua_string=%d get_lua_globals=%d",
            TAG, HAS(load_lua_file), HAS(load_lua_string), HAS(get_lua_globals));
        api.log_info("%s availability: get_sol_state_view=%d free_sol_object=%d free_sol_state_view=%d",
            TAG, HAS(get_sol_state_view), HAS(free_sol_object), HAS(free_sol_state_view));
        api.log_info("%s availability: exec_lua_chunk=%d get_global_string=%d set_global_string=%d",
            TAG, HAS(exec_lua_chunk), HAS(get_global_string), HAS(set_global_string));
        api.log_info("%s availability: synchronize_lua_event=%d reload_plugins=%d attempt_unload_plugins=%d reset_lua_scripts=%d",
            TAG, HAS(synchronize_lua_event), HAS(reload_plugins),
            HAS(attempt_unload_plugins), HAS(reset_lua_scripts));
        api.log_info("%s availability: on_lua_state_destroyed=%d on_imgui_frame=%d lock_lua=%d unlock_lua=%d",
            TAG, HAS(on_lua_state_destroyed), HAS(on_imgui_frame),
            HAS(lock_lua), HAS(unlock_lua));

        if (HAS(on_imgui_frame)) {
            fns->on_imgui_frame([](UEVR_ImGuiFrameCbData*) { ++g_imgui_frames; });
        }
        if (HAS(on_lua_state_destroyed)) {
            fns->on_lua_state_destroyed([](lua_State*) {
                API::get()->log_info("%s lua state destroyed (reset/unload)", TAG);
            });
        }
        if (HAS(set_global_string)) {
            fns->set_global_string("api_test_marker", "hello-from-cpp");
        }

        api.log_info("%s on_initialize done; press F7 in-overlay for runtime tests", TAG);
    }

    bool on_message(HWND, UINT msg, WPARAM wparam, LPARAM) override {
        if (msg == WM_KEYDOWN && wparam == HOTKEY) {
            run_runtime_tests();
        }
        return true;
    }

    void on_post_engine_tick(API::UGameEngine*, float) override {
        ++g_tick_counter;
        if (!g_did_runtime_tests && g_tick_counter == 60) {
            run_runtime_tests();
            g_did_runtime_tests = true;
        }
    }

    void on_custom_event(const char* name, const char* data) override {
        API::get()->log_info("%s on_custom_event name=%s data=%s", TAG,
            name ? name : "(null)", data ? data : "(null)");
    }

private:
    void run_runtime_tests() {
        auto& api = *API::get();
        const auto fns = api.param()->functions;

        api.log_info("%s ---- runtime tests start (tick=%llu imgui_frames=%llu) ----",
            TAG, (unsigned long long)g_tick_counter, (unsigned long long)g_imgui_frames);

        test_lua(fns);
        test_sdk();
        test_vr_angular_velocity();
        test_custom_event(fns);

        api.log_info("%s ---- runtime tests end ----", TAG);
    }

    void test_lua(const UEVR_PluginFunctions* fns) {
        auto& api = *API::get();

        if (HAS(get_global_string)) {
            char buf[128] = {};
            if (fns->get_global_string("api_test_marker", buf, sizeof(buf))) {
                api.log_info("%s get_global_string(api_test_marker)='%s'", TAG, buf);
            } else {
                api.log_warn("%s get_global_string returned false (lua not ready?)", TAG);
            }
        }

        if (HAS(exec_lua_chunk)) {
            char result[256] = {};
            const bool ok = fns->exec_lua_chunk(
                "return tostring(_G.api_test_marker or 'nil') .. '|' .. _VERSION",
                "api_test.exec_chunk", result, sizeof(result));
            api.log_info("%s exec_lua_chunk ok=%d result='%s'", TAG, (int)ok, result);
        }

        if (HAS(load_lua_string)) {
            fns->load_lua_string(
                "function api_test_echo(x) return 'echo:'..tostring(x) end",
                "api_test.load_string");

            if (HAS(exec_lua_chunk)) {
                char echo_result[128] = {};
                fns->exec_lua_chunk("return api_test_echo(42)",
                    "api_test.echo", echo_result, sizeof(echo_result));
                api.log_info("%s load_lua_string+call result='%s'", TAG, echo_result);
            }
        }

        if (HAS(lock_lua) && HAS(unlock_lua) && HAS(set_global_string)) {
            API::LuaLock lock;
            fns->set_global_string("api_test_locked_marker", "set-under-lock");
        }

        if (HAS(synchronize_lua_event)) {
            fns->synchronize_lua_event(nullptr, "api_test_event", "payload-from-cpp");
        }
    }

    void test_sdk() {
        auto& api = *API::get();
        const auto sdk_fns = api.sdk()->functions;

        if (API::UEngine::get() == nullptr) {
            api.log_warn("%s UEngine null; skipping SDK component tests", TAG);
            return;
        }

        auto pawn_class = api.find_uobject<API::UClass>(L"Class /Script/Engine.Pawn");
        auto actor = pawn_class ? API::UObjectHook::get_first_object_by_class(pawn_class) : nullptr;
        if (actor == nullptr) {
            api.log_warn("%s no Pawn found; skipping component tests", TAG);
            return;
        }

        auto comp_class = api.find_uobject<API::UClass>(L"Class /Script/Engine.SceneComponent");
        if (comp_class == nullptr) {
            api.log_warn("%s SceneComponent class not found", TAG);
            return;
        }

        if (sdk_fns->get_or_add_component != nullptr) {
            auto comp = sdk_fns->get_or_add_component(
                (UEVR_UObjectHandle)actor, (UEVR_UClassHandle)comp_class);
            api.log_info("%s get_or_add_component(SceneComponent on %p) -> %p",
                TAG, (void*)actor, (void*)comp);

            if (comp != nullptr && sdk_fns->attach_to != nullptr) {
                auto root = actor->get_property<API::UObject*>(L"RootComponent");
                if (root != nullptr) {
                    auto attached = sdk_fns->attach_to(comp, (UEVR_UObjectHandle)root);
                    api.log_info("%s attach_to root=%p -> %p",
                        TAG, (void*)root, (void*)attached);
                }
            }
        } else {
            api.log_warn("%s sdk.get_or_add_component NULL; skipping", TAG);
        }

        auto actor_class = api.find_uobject<API::UClass>(L"Class /Script/Engine.Actor");
        if (actor_class != nullptr) {
            auto prop = actor_class->find_property(L"RootComponent");
            if (prop != nullptr) {
                // set_property_flags has no NULL guard in the C++ wrapper, but
                // the underlying fptr could still be missing. Snapshot it.
                const uint64_t flags = prop->get_property_flags();
                prop->set_property_flags(flags | 0x1ULL);
                api.log_info("%s set_property_flags Actor.RootComponent old=0x%llx new=0x%llx",
                    TAG, (unsigned long long)flags,
                    (unsigned long long)(flags | 0x1ULL));
            }
        }
    }

    void test_vr_angular_velocity() {
        auto& api = *API::get();
        const auto vr = api.param()->vr;

        if (!vr || !vr->is_hmd_active()) {
            api.log_info("%s HMD inactive; skipping angular velocity test", TAG);
            return;
        }
        if (vr->get_angular_velocity == nullptr) {
            api.log_warn("%s vr.get_angular_velocity NULL; skipping", TAG);
            return;
        }

        const auto idx = vr->get_right_controller_index();
        if (idx == 0) {
            api.log_info("%s no right controller; skipping", TAG);
            return;
        }

        UEVR_Vector3f w{};
        vr->get_angular_velocity(idx, &w);
        api.log_info("%s right controller angular_velocity = (%.3f, %.3f, %.3f) rad/s",
            TAG, w.x, w.y, w.z);
    }

    void test_custom_event(const UEVR_PluginFunctions* fns) {
        if (fns->dispatch_custom_event != nullptr) {
            fns->dispatch_custom_event("api_test/ping", "from api_test_plugin");
        }
    }
};

static ApiTestPlugin g_plugin;
