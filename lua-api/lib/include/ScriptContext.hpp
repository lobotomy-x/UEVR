#pragma once

#include <iostream>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "ScriptPrerequisites.hpp"
#include <uevr/API.hpp>
#include <safetyhook.hpp>

#include <vector>

namespace uevr {
class ScriptContext : public std::enable_shared_from_this<ScriptContext> {
public:
    static std::shared_ptr<ScriptContext> create(lua_State* l, UEVR_PluginInitializeParam* param = nullptr) {
        auto ctx = std::shared_ptr<ScriptContext>(new ScriptContext(l, param));
        ctx->initialize();

        return ctx;
    }

    static std::shared_ptr<ScriptContext> create(std::shared_ptr<sol::state> l, UEVR_PluginInitializeParam* param = nullptr) {
        auto ctx = std::shared_ptr<ScriptContext>(new ScriptContext(l->lua_state(), param));
        ctx->initialize(l);

        return ctx;
    }

    ScriptContext() = delete;
    virtual ~ScriptContext();

    int setup_bindings();
    void setup_callback_bindings();

    bool valid() { return m_plugin_initialize_param != nullptr; }

    auto& lua() { return m_lua; }
    struct UE_ProxyPtr;
    UEVR_PluginInitializeParam* plugin_initialize_param() { return m_plugin_initialize_param; }

    sol::protected_function_result handle_protected_result(sol::protected_function_result result) {
        if (result.valid()) {
            return result;
        }

        sol::script_default_on_error(m_lua.lua_state(), std::move(result));
        return result;
    }

    static void log(const std::string& message);
    void log_error(const std::string& message) {
        log(message);

        std::unique_lock _{m_script_error_mutex};
        m_last_script_error_state.e = message;
        m_last_script_error_state.t = std::chrono::system_clock::now();
    }

    template <typename T1, typename T2> void add_callback(T1&& adder, T2&& cb) {
        std::scoped_lock _{m_mtx};

        if (m_plugin_initialize_param != nullptr) {
            adder(cb);
            s_callbacks_to_remove.push_back((void*)cb);
        }
    }

    auto& get_mutex() { return m_mtx; }

    void script_reset() {
        std::scoped_lock _{m_mtx};

        for (auto& cb : m_on_script_reset_callbacks)
            try {
                handle_protected_result(cb());
            } catch (const std::exception& e) {
                log_error("Exception in on_script_reset: " + std::string(e.what()));
            } catch (...) {
                log_error("Unknown exception in on_script_reset");
            }
    }

    void frame() {
        std::scoped_lock _{m_mtx};

        for (auto& cb : m_on_frame_callbacks)
            try {
                handle_protected_result(cb());
            } catch (const std::exception& e) {
                log_error("Exception in on_frame: " + std::string(e.what()));
            } catch (...) {
                log_error("Unknown exception in on_frame");
            }
    }

    void draw_ui() {
        std::scoped_lock _{m_mtx};

        for (auto& cb : m_on_draw_ui_callbacks)
            try {
                handle_protected_result(cb());
            } catch (const std::exception& e) {
                log_error("Exception in on_draw_ui: " + std::string(e.what()));
            } catch (...) {
                log_error("Unknown exception in on_draw_ui");
            }
    }

    void dispatch_event(std::string_view event_name, std::string_view event_data) {
        std::scoped_lock _{m_mtx};

        for (auto& cb : m_on_lua_event_callbacks)
            try {
                handle_protected_result(cb(event_name, event_data));
            } catch (const std::exception& e) {
                log_error("Exception in on_lua_event: " + std::string(e.what()));
            } catch (...) {
                log_error("Unknown exception in on_lua_event");
            }
    }

    struct ScriptErrorState {
        std::string e{};
        std::chrono::system_clock::time_point t{};
    };

    auto get_last_script_error() const {
        std::shared_lock _{m_script_error_mutex};
        return m_last_script_error_state;
    }

private:
    // Private constructor to prevent direct instantiation
    ScriptContext(lua_State* l, UEVR_PluginInitializeParam* param = nullptr);
    void initialize(std::shared_ptr<sol::state> l = nullptr);

    static inline std::vector<void*> s_callbacks_to_remove{};
    static inline std::mutex s_callbacks_to_remove_mtx{};

    sol::state_view m_lua;
    std::shared_ptr<sol::state> m_lua_shared{}; // This allows us to keep the state alive (if it was created by ScriptState)
    ScriptErrorState m_last_script_error_state{};
    mutable std::shared_mutex m_script_error_mutex{};

    std::recursive_mutex m_mtx{};
    UEVR_PluginInitializeParam* m_plugin_initialize_param{nullptr};
    std::vector<sol::protected_function> m_on_xinput_get_state_callbacks{};
    std::vector<sol::protected_function> m_on_xinput_set_state_callbacks{};
    std::vector<sol::protected_function> m_on_pre_engine_tick_callbacks{};
    std::vector<sol::protected_function> m_on_post_engine_tick_callbacks{};
    // C++-side change detection (polled each tick) so scripts can react without per-frame Lua polling.
    std::vector<sol::protected_function> m_on_pawn_changed_callbacks{};
    std::vector<sol::protected_function> m_on_view_target_changed_callbacks{};
    std::vector<sol::protected_function> m_on_level_changed_callbacks{};
    void* m_last_pawn{nullptr};
    void* m_last_view_target{nullptr};
    void* m_last_level{nullptr};
    std::vector<sol::protected_function> m_on_pre_slate_draw_window_render_thread_callbacks{};
    std::vector<sol::protected_function> m_on_post_slate_draw_window_render_thread_callbacks{};
    std::vector<sol::protected_function> m_on_early_calculate_stereo_view_offset_callbacks{};
    std::vector<sol::protected_function> m_on_pre_calculate_stereo_view_offset_callbacks{};
    std::vector<sol::protected_function> m_on_post_calculate_stereo_view_offset_callbacks{};
    std::vector<sol::protected_function> m_on_pre_viewport_client_draw_callbacks{};
    std::vector<sol::protected_function> m_on_post_viewport_client_draw_callbacks{};
    std::vector<sol::protected_function> m_on_lua_event_callbacks{};

    // Custom UEVR callbacks
    std::vector<sol::protected_function> m_on_frame_callbacks{};
    std::vector<sol::protected_function> m_on_draw_ui_callbacks{};
    std::vector<sol::protected_function> m_on_script_reset_callbacks{};
    std::vector<sol::protected_function> process_event_hooks{};

    struct UFunctionHookState {
        std::vector<sol::protected_function> pre_hooks{};
        std::vector<sol::protected_function> post_hooks{};

    };

    std::shared_mutex m_ufunction_hooks_mtx{};
    std::unordered_map<uevr::API::UFunction*, std::unique_ptr<UFunctionHookState>> m_ufunction_hooks{};

public:
    // Lua-owned safetyhook MidHook. The C-side destination is a single shared dispatcher; it
    // finds the right LuaMidHook by looking up whose trampoline contains the current
    // Context::rip on entry. Ownership is per-ScriptContext (so a state reset tears its hooks
    // down with it), and every live hook is also registered in a process-global vector so the
    // dispatcher can resolve hooks belonging to any state.
    //
    // Note: full "inline" hooks (replace the entire function) would need per-signature thunk
    // generation - they're not safely callable from Lua without knowing the calling convention
    // and return type up front. MidHook gives Lua scripts register-level read/write access
    // before the original instructions execute, which covers nearly every practical use case.
    struct LuaMidHook {
        SafetyHookMid hook;
        sol::protected_function callback;
        std::weak_ptr<ScriptContext> owner;
        uintptr_t target_addr{};
    };

    // Returns the LuaMidHook (also retained in the owning ScriptContext); nullptr on failure.
    std::shared_ptr<LuaMidHook> create_mid_hook(uintptr_t target, sol::protected_function cb);
    bool remove_mid_hook(uintptr_t target);

private:
    std::shared_mutex m_mid_hooks_mtx{};
    std::unordered_map<uintptr_t, std::shared_ptr<LuaMidHook>> m_mid_hooks{};

    // Process-global registry the C dispatcher uses to resolve a hook from ctx.rip. A shared_ptr
    // keeps the hook alive even if its ScriptContext goes away mid-dispatch.
    static inline std::shared_mutex s_all_mid_hooks_mtx{};
    static inline std::vector<std::shared_ptr<LuaMidHook>> s_all_mid_hooks{};

    static void global_mid_hook_dispatcher(safetyhook::Context& ctx);
    void invoke_mid_hook(LuaMidHook& h, safetyhook::Context& ctx);

    static bool global_ufunction_pre_handler(uevr::API::UFunction* fn, uevr::API::UObject* obj, void* params, void* result);
    static void global_ufunction_post_handler(uevr::API::UFunction* fn, uevr::API::UObject* obj, void* params, void* result);

    static void on_xinput_get_state(uint32_t* retval, uint32_t user_index, void* state);
    static void on_xinput_set_state(uint32_t* retval, uint32_t user_index, void* vibration);
    static void on_pre_engine_tick(UEVR_UGameEngineHandle engine, float delta_seconds);
    static void on_post_engine_tick(UEVR_UGameEngineHandle engine, float delta_seconds);
    static void on_pre_slate_draw_window_render_thread(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info);
    static void on_post_slate_draw_window_render_thread(UEVR_FSlateRHIRendererHandle renderer, UEVR_FViewportInfoHandle viewport_info);
    static void on_early_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle device, int view_index, float world_to_meters,
        UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double);
    static void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle device, int view_index, float world_to_meters,
        UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double);
    static void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle device, int view_index, float world_to_meters,
        UEVR_Vector3f* position, UEVR_Rotatorf* rotation, bool is_double);
    static void on_pre_viewport_client_draw(
        UEVR_UGameViewportClientHandle viewport_client, UEVR_FViewportHandle viewport, UEVR_FCanvasHandle canvas);
    static void on_post_viewport_client_draw(
        UEVR_UGameViewportClientHandle viewport_client, UEVR_FViewportHandle viewport, UEVR_FCanvasHandle canvas);
    static void on_frame();
    static void on_draw_ui();
    static void on_script_reset();                                                                                                                                                                                                                                                                                                                                 // Dump the current state of the Lua stack

    static void on_lua_event(std::string_view event_name, std::string_view event_data);


    auto dump_stack(lua_State*, const char* message = "") -> void;

    auto get_stack_dump(lua_State* lua_state, const char* message = "") -> std::string;


};
} // namespace uevr