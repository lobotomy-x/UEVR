// lua_imgui_plugin: a "proper" plugin that draws an ImGui window without
// linking imgui/cimgui from C++. It uses the newly-wired PluginFunctions
// (load_lua_string + exec_lua_chunk + set/get_global_string) to push a Lua
// chunk into UEVR's main script context. The Lua side registers
// uevr.sdk.callbacks.on_frame and uses UEVR's existing imgui Lua table to
// draw a multi-tab dev panel:
//
//   * REPL — multi-line Lua input + scrolling output history.
//   * UObject — quick class lookup by short name (resolves to full path).
//   * VR — live pose / angular-velocity readout for both controllers.
//   * Console — sends Unreal console commands.
//   * Bridge — round-trip demo for set/get_global_string.
//
// Data flow:
//   C++ side  ──set_global_string──>  Lua _G.lua_imgui_plugin_state
//   Lua side  ──button click──>       _G.lua_imgui_plugin_state = "..."
//   C++ side  ──get_global_string──>  reads back, drives behavior in tick
//
// Press F8 to re-push the Lua chunk (useful after Reset scripts). The
// "lua_imgui_plugin/cmd" custom event can also be dispatched from C++ to drive
// arbitrary actions from external plugins.

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {
constexpr const char* TAG = "[lua_imgui]";
constexpr WPARAM RELOAD_KEY = VK_F8;

constexpr const char* LUA_SOURCE = R"LUA(
-- lua_imgui_plugin: drawn from C++ via exec_lua_chunk.

if _G.__lua_imgui_plugin and _G.__lua_imgui_plugin.alive then
    -- Disarm the previous callback so we don't double-render.
    _G.__lua_imgui_plugin.alive = false
end

local M = {}
M.alive = true
M.tick = 0

-- Panel state -------------------------------------------------------------
M.tab = "repl"
M.tabs = { "repl", "uobject", "vr", "console", "bridge" }

-- REPL
M.repl_input = "return tostring(_VERSION)\n"
M.repl_output = {}    -- ring buffer
M.repl_output_max = 200
M.repl_history = {}
M.repl_history_idx = 0
M.repl_autoscroll = true

-- UObject inspector
M.uobj_query = "Pawn"
M.uobj_result = "(none yet)"

-- VR
M.vr_log_pose = false

-- Console
M.console_cmd = "stat fps"
M.console_history = {}

-- Bridge
M.bridge_text = "set-from-lua"

_G.__lua_imgui_plugin = M
_G.lua_imgui_plugin_state = "ready"

local imgui = imgui

local function repl_push(line)
    if #M.repl_output >= M.repl_output_max then
        table.remove(M.repl_output, 1)
    end
    table.insert(M.repl_output, line or "")
end

local function repl_run(src)
    if not src or src == "" then return end
    table.insert(M.repl_history, src)
    M.repl_history_idx = #M.repl_history + 1
    repl_push(">> " .. src:gsub("\n", " | "))

    -- Try as expression first ("return <src>"), fall back to statement.
    local fn, err = load("return (" .. src .. ")", "repl-expr", "t")
    if not fn then
        fn, err = load(src, "repl-stmt", "t")
    end
    if not fn then
        repl_push("!! parse: " .. tostring(err))
        return
    end
    local ok, ret = pcall(fn)
    if not ok then
        repl_push("!! error: " .. tostring(ret))
        return
    end
    if ret ~= nil then
        repl_push(" = " .. tostring(ret))
    else
        repl_push(" = nil")
    end
end

local function draw_repl()
    imgui.text("Multi-line Lua. Ctrl+Enter or 'Run' to execute.")
    local changed, new_text = imgui.input_text_multiline(
        "##repl_in", M.repl_input, { -1, 90 })
    if changed then M.repl_input = new_text end

    if imgui.button("Run") then
        repl_run(M.repl_input)
    end
    imgui.same_line()
    if imgui.button("Clear output") then
        M.repl_output = {}
    end
    imgui.same_line()
    local achg, av = imgui.checkbox("autoscroll", M.repl_autoscroll)
    if achg then M.repl_autoscroll = av end
    imgui.same_line()
    imgui.text(string.format("(%d lines)", #M.repl_output))

    imgui.separator()
    imgui.begin_child_window("##repl_out", { -1, 200 }, true)
    for _, line in ipairs(M.repl_output) do
        if line:sub(1,2) == "!!" then
            imgui.text_colored(line, 0xFF4040FF)
        elseif line:sub(1,2) == ">>" then
            imgui.text_colored(line, 0xFFFFFF40)
        else
            imgui.text(line)
        end
    end
    if M.repl_autoscroll and imgui.set_scroll_here_y then
        imgui.set_scroll_here_y(1.0)
    end
    imgui.end_child_window()
end

local function draw_uobject()
    imgui.text("Short class name, e.g. 'Pawn', 'PlayerController', 'Actor'.")
    local changed, q = imgui.input_text("query", M.uobj_query)
    if changed then M.uobj_query = q end
    imgui.same_line()
    if imgui.button("find class") then
        local full = "Class /Script/Engine." .. M.uobj_query
        local cls = uevr.api:find_uobject(full)
        if cls then
            M.uobj_result = full .. "  ->  " .. tostring(cls:get_full_name())
        else
            M.uobj_result = "not found: " .. full
        end
    end
    imgui.same_line()
    if imgui.button("first instance") then
        local full = "Class /Script/Engine." .. M.uobj_query
        local cls = uevr.api:find_uobject(full)
        if cls then
            local objs = uevr.find_uobjects_by_class(cls, true)
            if objs and #objs > 0 then
                M.uobj_result = string.format("%s  (#%d total)\nfirst: %s",
                    M.uobj_query, #objs, tostring(objs[1]:get_full_name()))
            else
                M.uobj_result = "no instances of " .. M.uobj_query
            end
        else
            M.uobj_result = "class not found"
        end
    end

    imgui.separator()
    imgui.text_wrapped(M.uobj_result or "")
end

local function vr_vec3(v)
    if not v then return "(nil)" end
    return string.format("(%.2f, %.2f, %.2f)", v.x or 0, v.y or 0, v.z or 0)
end

local function draw_vr()
    local vr = uevr.params.vr
    if not vr then imgui.text("vr api unavailable"); return end

    imgui.text(string.format("HMD active: %s | OpenXR: %s | OpenVR: %s",
        tostring(vr.is_hmd_active()), tostring(vr.is_openxr()), tostring(vr.is_openvr())))

    local function pose_block(label, idx_fn)
        local idx = idx_fn()
        if idx == 0 then imgui.text(label .. ": not connected"); return end
        local pos, rot = uevr.params.vr.get_pose(idx)
        imgui.text(string.format("%s [%d] pos=%s", label, idx, vr_vec3(pos)))
        if rot then
            imgui.text(string.format("  rot=(w=%.2f x=%.2f y=%.2f z=%.2f)",
                rot.w or 0, rot.x or 0, rot.y or 0, rot.z or 0))
        end
    end

    pose_block("HMD",   uevr.params.vr.get_hmd_index)
    pose_block("LEFT",  uevr.params.vr.get_left_controller_index)
    pose_block("RIGHT", uevr.params.vr.get_right_controller_index)

    local chg, lp = imgui.checkbox("log poses to file every 60 frames", M.vr_log_pose)
    if chg then M.vr_log_pose = lp end
end

local function draw_console()
    imgui.text("Run Unreal console commands via the local player controller.")
    local changed, c = imgui.input_text("cmd", M.console_cmd)
    if changed then M.console_cmd = c end
    imgui.same_line()
    if imgui.button("send") then
        local ok = pcall(function()
            local engine = uevr.api:get_engine()
            local viewport = engine:get_property("GameViewport")
            if viewport then viewport:exec(M.console_cmd) end
        end)
        table.insert(M.console_history, 1,
            string.format("[%s] %s", ok and "OK" or "ERR", M.console_cmd))
        if #M.console_history > 30 then table.remove(M.console_history) end
    end

    imgui.separator()
    imgui.begin_child_window("##console_hist", { -1, 180 }, true)
    for _, h in ipairs(M.console_history) do imgui.text(h) end
    imgui.end_child_window()
end

local function draw_bridge()
    imgui.text("Round-trip channel _G.lua_imgui_plugin_state <-> C++ DLL.")
    imgui.text(string.format("current: '%s'", tostring(_G.lua_imgui_plugin_state)))

    local changed, t = imgui.input_text("value", M.bridge_text)
    if changed then M.bridge_text = t end

    if imgui.button("write -> _G") then
        _G.lua_imgui_plugin_state = M.bridge_text
    end
    imgui.same_line()
    if imgui.button("dispatch custom event") then
        if uevr.params.functions.dispatch_custom_event then
            uevr.params.functions.dispatch_custom_event(
                "lua_imgui_plugin/cmd", M.bridge_text)
        end
    end
    imgui.same_line()
    if imgui.button("dispatch lua event") then
        uevr.params.functions.dispatch_lua_event(
            "lua_imgui_plugin/lua_evt", M.bridge_text)
    end
end

local draw_funcs = {
    repl = draw_repl,
    uobject = draw_uobject,
    vr = draw_vr,
    console = draw_console,
    bridge = draw_bridge,
}

uevr.sdk.callbacks.on_draw_ui(function()
    if not M.alive then return end
    M.tick = M.tick + 1

    if imgui.collapsing_header("lua_imgui_plugin [C++ driver]") then
        imgui.text(string.format("frame=%d  lua=%s", M.tick, _VERSION))
        imgui.separator()

        -- Tab strip
        for i, t in ipairs(M.tabs) do
            if M.tab == t then
                imgui.text_colored("[" .. t .. "]", 0xFF40FFFF)
            else
                if imgui.button(t) then M.tab = t end
            end
            if i < #M.tabs then imgui.same_line() end
        end
        imgui.separator()

        local fn = draw_funcs[M.tab]
        if fn then
            local ok, err = pcall(fn)
            if not ok then
                imgui.text_colored("draw error: " .. tostring(err), 0xFF4040FF)
            end
        end
    end
end)

-- React to C++ custom events ("lua_imgui_plugin/cmd"): treat data as Lua to run.
uevr.sdk.callbacks.on_lua_event(function(name, data)
    -- name+data come in via dispatch_lua_event from elsewhere
    if name == "lua_imgui_plugin/run" then
        repl_run(data or "")
    end
end)

return "lua_imgui_plugin v2 loaded"
)LUA";

uint64_t g_tick = 0;
uint64_t g_imgui_frames = 0;
bool g_pushed = false;
}

class LuaImguiPlugin : public Plugin {
public:
    void on_initialize() override {
        auto& api = *API::get();
        const auto fns = api.param()->functions;

        api.log_info("%s init. branch=%s commit=%s", TAG,
            fns->get_branch(), fns->get_commit_hash());

        if (fns->exec_lua_chunk == nullptr) {
            api.log_warn("%s exec_lua_chunk NULL — rebuild UEVRBackend.dll", TAG);
            return;
        }

        if (fns->on_imgui_frame != nullptr) {
            fns->on_imgui_frame([](UEVR_ImGuiFrameCbData*) { ++g_imgui_frames; });
        }

        if (fns->on_lua_state_destroyed != nullptr) {
            fns->on_lua_state_destroyed([](lua_State*) {
                API::get()->log_info("%s lua state destroyed; will auto-repush", TAG);
                g_pushed = false;
            });
        }

        api.log_info("%s ready; auto-push on tick 30 (or F8 manual)", TAG);
    }

    bool on_message(HWND, UINT msg, WPARAM wparam, LPARAM) override {
        if (msg == WM_KEYDOWN && wparam == RELOAD_KEY) {
            API::get()->log_info("%s F8 -> re-push lua chunk", TAG);
            push_lua();
        }
        return true;
    }

    void on_post_engine_tick(API::UGameEngine*, float) override {
        ++g_tick;
        if (!g_pushed && g_tick > 30) {
            push_lua();
        }

        // Periodic round-trip log: pull state from lua side.
        if (g_pushed && (g_tick % 600) == 0) {
            const auto fns = API::get()->param()->functions;
            if (fns->get_global_string != nullptr) {
                char buf[128] = {};
                if (fns->get_global_string("lua_imgui_plugin_state", buf, sizeof(buf))) {
                    API::get()->log_info("%s heartbeat state='%s' tick=%llu imgui_frames=%llu",
                        TAG, buf, (unsigned long long)g_tick,
                        (unsigned long long)g_imgui_frames);
                }
            }
        }
    }

    void on_custom_event(const char* name, const char* data) override {
        API::get()->log_info("%s custom_event '%s' '%s'", TAG,
            name ? name : "", data ? data : "");

        // Route "lua_imgui_plugin/run" custom events into the REPL by
        // synthesizing a lua dispatch.
        if (name && std::strcmp(name, "lua_imgui_plugin/run") == 0) {
            const auto fns = API::get()->param()->functions;
            if (fns->dispatch_lua_event != nullptr) {
                fns->dispatch_lua_event("lua_imgui_plugin/run", data ? data : "");
            }
        }
    }

private:
    void push_lua() {
        auto& api = *API::get();
        const auto fns = api.param()->functions;

        char result[128] = {};
        const bool ok = fns->exec_lua_chunk(LUA_SOURCE, "lua_imgui_plugin.lua",
            result, sizeof(result));
        api.log_info("%s exec_lua_chunk ok=%d result='%s'", TAG, (int)ok, result);
        g_pushed = ok;
    }
};

static LuaImguiPlugin g_plugin;
