-- world_anchor_demo.lua — draws a marker + label that sticks to a world-space
-- point, staying correctly aligned in VR even while the UEVR UI is open.
--
-- Demonstrates `uevr.api_fast.world_to_screen_overlay`: the same flat
-- projection as `world_to_screen`, but remapped onto the framework overlay
-- quad when an HMD is active (the same correction the built-in gizmo uses).
-- With no HMD it behaves exactly like `world_to_screen`.
--
-- Anchor used here: the local pawn's actor location (always available).

print("Initializing world_anchor_demo.lua")

local api_fast = uevr.api_fast

local MARKER_COLOR = 0xFF00FFFF -- ABGR: opaque yellow
local TEXT_COLOR   = 0xFFFFFFFF
local nonoverlay
uevr.sdk.callbacks.on_frame(function()
    local pc = api_fast.get_player_controller(0)
    if pc == nil then return end

    local pawn = api_fast.get_local_pawn(0)
    if pawn == nil then return end

    local loc = api_fast.get_actor_location(pawn)
    if loc == nil then return end

    -- VR-aware projection: stays glued to the world point with the UI open.
    local p = api_fast.world_to_screen_overlay(pc, loc)
    if p == nil or (p.x == 0 and p.y == 0) then
    	p = api_fast.world_to_screen(pc, loc)
    	 nonoverlay = true
    return end -- projection failed / off-screen

    local dl = imgui.get_background_draw_list()
    if dl == nil then return end

    dl:add_circle_filled({p.x, p.y}, 6.0, MARKER_COLOR, 12)
    dl:add_text({p.x + 10, p.y - 6}, TEXT_COLOR, "pawn anchor (world-aligned)")
    if nonoverlay then dl:add_text({p.x + 10, p.y + 16}, TEXT_COLOR, "not overlay") end
end)
