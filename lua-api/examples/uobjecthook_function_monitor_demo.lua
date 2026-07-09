-- uobjecthook_function_monitor_demo.lua — live overlay for the "function
-- monitor" Lua event, and a demonstration that hook_ptr() from Lua and the
-- Block/Monitor toggle in UObjectHook's own UI share ONE pool: hooking a
-- function from this script makes it show up as "[Mon]"/"[Blocked]" in the
-- Class Browser / Function Hooks windows, and toggling Block/Monitor on a
-- function from the UI fires this same event back into Lua.
--
-- Event: "uobjecthook_function_monitor", fired via uevr.sdk.callbacks.on_lua_event.
-- JSON payload: { "address":hex, "full_name", "added":bool, "blocked":bool,
--   "monitored":bool, "call_count":N, "source" ("block"|"monitor"|"external_hook"|"unhooked"),
--   "top_classes": [{"name","count"}, ...], "top_callers": [{"address","full_name","count"}, ...] }
--
-- "source" tells you WHY this fired: "block"/"monitor" = UI toggle, "external_hook" =
-- a hook_ptr() call (this script or a native plugin) UObjectHook noticed on its
-- per-frame sync, "unhooked" = the hook was removed.

print("Initializing uobjecthook_function_monitor_demo.lua")

local pool = {} -- address (string) -> last payload table
local pool_order = {}
local event_count = 0

local function remove_from_order(addr)
    for i, a in ipairs(pool_order) do
        if a == addr then
            table.remove(pool_order, i)
            return
        end
    end
end

uevr.sdk.callbacks.on_lua_event(function(name, data)
    if name ~= "uobjecthook_function_monitor" then return end

    event_count = event_count + 1

    local ok, decoded = pcall(json.load_string, data)
    if not ok or decoded == nil then
        print("[function_monitor_demo] failed to parse payload: " .. tostring(decoded))
        return
    end

    local addr = decoded.address
    if addr == nil then return end

    if decoded.added then
        if pool[addr] == nil then
            table.insert(pool_order, addr)
        end
        pool[addr] = decoded
        print(string.format("[function_monitor_demo] #%d %s via %s: %s (calls=%d, blocked=%s, monitored=%s)",
            event_count, addr, decoded.source or "?", decoded.full_name or "?",
            decoded.call_count or 0, tostring(decoded.blocked), tostring(decoded.monitored)))

        for _, c in ipairs(decoded.top_classes or {}) do
            print(string.format("    class  %-40s x%d", c.name, c.count))
        end
        for _, c in ipairs(decoded.top_callers or {}) do
            print(string.format("    caller %-40s x%d (0x%s)", c.full_name, c.count, c.address))
        end
    else
        print(string.format("[function_monitor_demo] #%d %s unhooked: %s",
            event_count, addr, (pool[addr] and pool[addr].full_name) or "?"))
        pool[addr] = nil
        remove_from_order(addr)
    end
end)

uevr.sdk.callbacks.on_draw_ui(function()
    imgui.begin_window("Function Monitor Demo")
    imgui.text(string.format("Events received: %d", event_count))
    imgui.text(string.format("Currently in pool: %d", #pool_order))
    imgui.separator()

    for _, addr in ipairs(pool_order) do
        local f = pool[addr]
        local tag = f.blocked and "[BLOCKED]" or (f.monitored and "[MON]" or ("[" .. (f.source or "?") .. "]"))
        imgui.text(string.format("%s %s", tag, f.full_name or "?"))
        imgui.text(string.format("  calls=%d addr=0x%s", f.call_count or 0, addr))
    end

    imgui.end_window()
end)

-- Demonstrates the OTHER direction: hook a function straight from Lua via
-- fn:hook_ptr(...) and watch it show up in this same pool (source="external_hook")
-- as well as tagged in the Class Browser / Function Hooks UI, with zero UObjectHook
-- UI interaction. Commented out by default — uncomment to try it.
--
-- local api = uevr.api
-- local fn = api:find_uobject("Function /Script/Engine.Actor.GetActorLocation")
-- if fn ~= nil then
--     fn:hook_ptr(
--         function(fn, obj, locals) print("[function_monitor_demo] pre GetActorLocation") end,
--         function(fn, obj, locals, ret) print("[function_monitor_demo] post GetActorLocation") end
--     )
-- end

uevr.sdk.callbacks.on_script_reset(function()
    print("Resetting uobjecthook_function_monitor_demo.lua")
end)
