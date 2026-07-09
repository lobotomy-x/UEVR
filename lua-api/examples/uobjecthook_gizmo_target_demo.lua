-- uobjecthook_gizmo_target_demo.lua — live overlay for the "gizmo target" Lua
-- event UObjectHook fires whenever a component is added to / removed from the
-- active gizmo selection (click-select commit, the "Show gizmo" checkbox, or
-- "Remove from selection"). Intended as the hook point for a Lua-side overlay
-- material / debug-visualization feature keyed off gizmo selection.
--
-- Event: "uobjecthook_gizmo_target", fired via uevr.sdk.callbacks.on_lua_event.
-- JSON payload: { "address":hex, "full_name", "added":bool }
--
-- This demo just keeps a set of currently-selected addresses and prints /
-- displays it — swap the print()/imgui calls for e.g. a
-- uevr.api:to_uobject(tonumber(addr,16)) + material override to build the
-- real overlay-highlight feature entirely from Lua.

print("Initializing uobjecthook_gizmo_target_demo.lua")

local selected = {} -- address (string) -> full_name (string)
local selected_order = {} -- preserves insertion order for display
local event_count = 0

local function remove_from_order(addr)
    for i, a in ipairs(selected_order) do
        if a == addr then
            table.remove(selected_order, i)
            return
        end
    end
end

uevr.sdk.callbacks.on_lua_event(function(name, data)
    if name ~= "uobjecthook_gizmo_target" then return end

    event_count = event_count + 1

    local ok, decoded = pcall(json.load_string, data)
    if not ok or decoded == nil then
        print("[gizmo_target_demo] failed to parse payload: " .. tostring(decoded))
        return
    end

    local addr = decoded.address
    if addr == nil then return end

    if decoded.added then
        if selected[addr] == nil then
            table.insert(selected_order, addr)
        end
        selected[addr] = decoded.full_name or "?"
        print(string.format("[gizmo_target_demo] #%d + %s (0x%s)", event_count, selected[addr], addr))
    else
        print(string.format("[gizmo_target_demo] #%d - %s (0x%s)", event_count, selected[addr] or "?", addr))
        selected[addr] = nil
        remove_from_order(addr)
    end
end)

uevr.sdk.callbacks.on_draw_ui(function()
    imgui.begin_window("Gizmo Target Demo")
    imgui.text(string.format("Events received: %d", event_count))
    imgui.text(string.format("Currently selected: %d", #selected_order))
    imgui.separator()

    for _, addr in ipairs(selected_order) do
        imgui.text(string.format("0x%s  %s", addr, selected[addr] or "?"))
    end

    imgui.end_window()
end)

uevr.sdk.callbacks.on_script_reset(function()
    print("Resetting uobjecthook_gizmo_target_demo.lua")
end)
