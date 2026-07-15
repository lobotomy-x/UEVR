// Function block/monitor system: the persistent pre/post hook installers, the block/monitor
// state (is_func_blocked/is_func_monitored/func_call_count + their file-scoped g_blocked_funcs/
// g_monitored_funcs/g_func_call_counts backing sets), dispatch_function_monitor_event (fires the
// Lua uobjecthook_function_monitor event), sync_hooked_functions_to_lua, and draw_active_function_hooks
// — split out of UObjectHook.cpp purely for file-size organization (that file was 13k lines). Still
// UObjectHook:: member function definitions (same class, same members, same mutex, same everything)
// — only WHERE the code lives changed, not what it does. See src/mods/uobjecthook/SDKDumper.cpp for
// the existing precedent of this pattern, and Gizmo.cpp / PropertyEditor.cpp / ClassBrowser.cpp for
// the earlier splits of this kind.
//
// ui_function_context_menu and ui_handle_functions deliberately stay in UObjectHook.cpp: they call
// render_function_call/load_live_caller_slot, which are tied to the "Live Function Caller" parameter-
// editor machinery (ParamEditState and friends) — file-local struct types not visible from the header,
// so that machinery can't cleanly promote/relocate the way the smaller helpers in the other splits did.

#include <sstream>
#include <cctype>
#include <algorithm>

#include <utility/Logging.hpp>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/UObjectBase.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FField.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/AActor.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/USceneComponent.hpp>

#include <imgui_internal.h>
#include "../PluginLoader.hpp"
#include "../LuaLoader.hpp"

#include "../UObjectHook.hpp"

namespace {
// Forward declaration: defined below (after is_func_blocked/is_func_monitored, which it calls) —
// set_func_blocked/set_func_monitored need to call it at their point of state change.
void dispatch_function_monitor_event(sdk::UFunction* fn, bool added, const char* source);

// User-blocked UFunctions. The pre-hook below returns false for any function in
// this set, which makes PluginLoader::ufunction_hook_intermediary skip the
// original call (the function becomes a no-op). The hook stays installed when a
// function is unblocked — it simply returns true again — so toggling needs no
// unhook path.
std::unordered_set<sdk::UFunction*> g_blocked_funcs{};
std::mutex g_blocked_funcs_mtx{};

bool uobjecthook_block_pre(UEVR_UFunctionHandle fn, UEVR_UObjectHandle, void*, void*) {
    std::scoped_lock _{g_blocked_funcs_mtx};
    return g_blocked_funcs.find((sdk::UFunction*)fn) == g_blocked_funcs.end();
}

// Per-function call monitor: a post-hook counts invocations for any monitored
// function. Same persistent-hook pattern as the blocker — unmonitoring just
// removes it from the set. Counts are read on the render thread, incremented on
// the game thread, so the map is mutex-guarded.
std::unordered_set<sdk::UFunction*> g_monitored_funcs{};
std::unordered_map<sdk::UFunction*, uint64_t> g_func_call_counts{};
std::mutex g_monitor_mtx{};

bool uobjecthook_monitor_post(UEVR_UFunctionHandle fn, UEVR_UObjectHandle, void*, void*) {
    std::scoped_lock _{g_monitor_mtx};
    auto f = (sdk::UFunction*)fn;
    if (g_monitored_funcs.find(f) != g_monitored_funcs.end()) {
        ++g_func_call_counts[f];
    }
    return true;
}

// Fires "uobjecthook_function_monitor" to Lua whenever a function's hooked/monitored/blocked state
// changes — from the UI (Block/Monitor toggle in ui_function_context_menu, via set_func_blocked/
// set_func_monitored above) OR from ANY external hook_ptr usage (a Lua script's fn:hook_ptr(...), or a
// native plugin) via the per-frame sync in UObjectHook::sync_hooked_functions_to_lua(). `source`
// documents which path triggered this ("block", "monitor", or "external_hook") so a script can tell
// UI-driven monitoring apart from its own hook. Caller info (top_classes/top_callers) comes from
// m_called_functions' per-receiver tallies — process_event_hook populates these for EVERY function it
// records regardless of how that function came to be hooked, as long as the global ProcessEvent
// listener is on.
// JSON payload: {"address":hex,"full_name","added":bool,"blocked":bool,"monitored":bool,
//   "call_count":N,"source","top_classes":[{"name","count"}],"top_callers":[{"address","full_name","count"}]}
void dispatch_function_monitor_event(sdk::UFunction* fn, bool added, const char* source) {
    if (fn == nullptr) {
        return;
    }
    try {
        char addr_buf[24];
        std::snprintf(addr_buf, sizeof(addr_buf), "%llx", (unsigned long long)(uintptr_t)fn);
        std::string full_name;
        try { full_name = utility::narrow(fn->get_full_name()); } catch (...) {}

        uint64_t call_count = 0;
        nlohmann::json top_classes = nlohmann::json::array();
        nlohmann::json top_callers = nlohmann::json::array();
        {
            auto hook = UObjectHook::get();
            std::scoped_lock _{hook->m_function_mutex};
            if (auto it = hook->m_called_functions.find(fn); it != hook->m_called_functions.end()) {
                call_count = it->second.call_count;

                std::vector<std::pair<sdk::UClass*, size_t>> classes(
                    it->second.caller_class_counts.begin(), it->second.caller_class_counts.end());
                std::sort(classes.begin(), classes.end(), [](auto& a, auto& b) { return a.second > b.second; });
                for (size_t i = 0; i < classes.size() && i < 5; ++i) {
                    std::string cname;
                    try { cname = utility::narrow(classes[i].first->get_fname().to_string()); } catch (...) { continue; }
                    top_classes.push_back({{"name", cname}, {"count", classes[i].second}});
                }

                std::vector<std::pair<sdk::UObject*, size_t>> callers(
                    it->second.caller_instance_counts.begin(), it->second.caller_instance_counts.end());
                std::sort(callers.begin(), callers.end(), [](auto& a, auto& b) { return a.second > b.second; });
                for (size_t i = 0; i < callers.size() && i < 5; ++i) {
                    auto* obj = callers[i].first;
                    if (!hook->exists((sdk::UObjectBase*)obj)) continue;
                    char caddr[24];
                    std::snprintf(caddr, sizeof(caddr), "%llx", (unsigned long long)(uintptr_t)obj);
                    std::string oname;
                    try { oname = utility::narrow(obj->get_full_name()); } catch (...) {}
                    top_callers.push_back({{"address", caddr}, {"full_name", oname}, {"count", callers[i].second}});
                }
            }
        }

        const nlohmann::json payload{
            {"address", addr_buf},
            {"full_name", full_name},
            {"added", added},
            {"blocked", UObjectHook::is_func_blocked(fn)},
            {"monitored", UObjectHook::is_func_monitored(fn)},
            {"call_count", call_count},
            {"source", source},
            {"top_classes", top_classes},
            {"top_callers", top_callers}
        };
        const auto data = payload.dump();
        LuaLoader::get()->dispatch_event("uobjecthook_function_monitor", data);
    } catch (const std::exception& e) {
        spdlog::error("[UObjectHook] dispatch_function_monitor_event failed: {}", e.what());
    } catch (...) {
        spdlog::error("[UObjectHook] dispatch_function_monitor_event failed");
    }
}
} // namespace
void UObjectHook::set_func_blocked(sdk::UFunction* fn, bool blocked) {
    if (blocked) {
        // hook_ufunction_ptr dedups the same pre-fn pointer, so this is idempotent.
        PluginLoader::get()->hook_ufunction_ptr((UEVR_UFunctionHandle)fn, &uobjecthook_block_pre, nullptr);
        std::scoped_lock _{g_blocked_funcs_mtx};
        g_blocked_funcs.insert(fn);
    } else {
        std::scoped_lock _{g_blocked_funcs_mtx};
        g_blocked_funcs.erase(fn);
    }
    dispatch_function_monitor_event(fn, blocked, "block");
}
void UObjectHook::set_func_monitored(sdk::UFunction* fn, bool on) {
    if (on) {
        PluginLoader::get()->hook_ufunction_ptr((UEVR_UFunctionHandle)fn, nullptr, &uobjecthook_monitor_post);
        std::scoped_lock _{g_monitor_mtx};
        g_monitored_funcs.insert(fn);
    } else {
        std::scoped_lock _{g_monitor_mtx};
        g_monitored_funcs.erase(fn);
    }
    dispatch_function_monitor_event(fn, on, "monitor");
}
bool UObjectHook::is_func_blocked(sdk::UFunction* fn) {
    std::scoped_lock _{g_blocked_funcs_mtx};
    return g_blocked_funcs.find(fn) != g_blocked_funcs.end();
}
bool UObjectHook::is_func_monitored(sdk::UFunction* fn) {
    std::scoped_lock _{g_monitor_mtx};
    return g_monitored_funcs.find(fn) != g_monitored_funcs.end();
}
uint64_t UObjectHook::func_call_count(sdk::UFunction* fn) {
    std::scoped_lock _{g_monitor_mtx};
    auto it = g_func_call_counts.find(fn);
    return it != g_func_call_counts.end() ? it->second : 0;
}

// Called once per frame from on_frame(). Diffs PluginLoader::get_hooked_functions() (the single choke
// point ANY hook_ptr call goes through — Lua's fn:hook_ptr(...), a native plugin, or UObjectHook's own
// Block/Monitor) against what's already been announced, and dispatches "uobjecthook_function_monitor"
// for anything new (source = "monitor"/"block" if the UI put it there, else "external_hook") or
// removed. This is what makes the Lua-visible pool bidirectional: a script's own fn:hook_ptr() call is
// picked up here exactly the same way a UI Block/Monitor toggle is (that path ALSO dispatches
// immediately from set_func_blocked/set_func_monitored — this loop is a safety net that additionally
// covers hooks the UI never touched).
void UObjectHook::sync_hooked_functions_to_lua() {
    auto current_vec = PluginLoader::get()->get_hooked_functions();
    std::unordered_set<sdk::UFunction*> current{current_vec.begin(), current_vec.end()};

    for (auto* fn : current) {
        if (fn == nullptr || m_known_hooked_funcs.contains(fn)) {
            continue;
        }
        const char* source = is_func_blocked(fn) ? "block" : (is_func_monitored(fn) ? "monitor" : "external_hook");
        dispatch_function_monitor_event(fn, true, source);
    }
    for (auto* fn : m_known_hooked_funcs) {
        if (fn != nullptr && !current.contains(fn)) {
            dispatch_function_monitor_event(fn, false, "unhooked");
        }
    }

    m_known_hooked_funcs = std::move(current);
}

void UObjectHook::draw_active_function_hooks() {
    // Snapshot under the locks, then render (don't hold a lock across ImGui).
    std::vector<sdk::UFunction*> blocked;
    std::vector<std::pair<sdk::UFunction*, uint64_t>> monitored;
    {
        std::scoped_lock _{g_blocked_funcs_mtx};
        blocked.assign(g_blocked_funcs.begin(), g_blocked_funcs.end());
    }
    {
        std::scoped_lock _{g_monitor_mtx};
        for (auto* f : g_monitored_funcs) {
            auto it = g_func_call_counts.find(f);
            monitored.emplace_back(f, it != g_func_call_counts.end() ? it->second : 0);
        }
    }

    if (blocked.empty() && monitored.empty()) {
        ImGui::TextDisabled("No blocked or monitored functions.");
        return;
    }

    const auto name_of = [](sdk::UFunction* f) -> std::string {
        try { return utility::narrow(f->get_full_name()); } catch (...) { return std::format("[{:#x}]", (uintptr_t)f); }
    };

    if (!blocked.empty()) {
        ImGui::SeparatorText("Blocked (no-op'd)");
        for (auto* f : blocked) {
            ImGui::PushID((void*)f);
            if (ImGui::SmallButton("Unblock")) {
                set_func_blocked(f, false);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(name_of(f).c_str());
            ImGui::PopID();
        }
    }

    if (!monitored.empty()) {
        ImGui::SeparatorText("Monitored");
        if (ImGui::SmallButton("Reset all counts")) {
            std::scoped_lock _{g_monitor_mtx};
            g_func_call_counts.clear();
        }
        for (auto& [f, count] : monitored) {
            ImGui::PushID((void*)f);
            if (ImGui::SmallButton("Stop")) {
                set_func_monitored(f, false);
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "%llu", (unsigned long long)count);
            ImGui::SameLine();
            ImGui::TextUnformatted(name_of(f).c_str());
            ImGui::PopID();
        }
    }
}
