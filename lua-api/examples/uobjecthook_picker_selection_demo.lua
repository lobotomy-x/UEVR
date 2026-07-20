-- uobjecthook_picker_selection_demo.lua — live overlay for the "picker selection"
-- Lua event UObjectHook fires while the click-select (scroll-to-pick) picker is
-- active. Draws the current candidate list on screen and logs every change.
--
-- Event: "uobjecthook_picker_selection", fired via uevr.sdk.callbacks.on_lua_event.
-- JSON payload — "targets" is reordered so index 1 (Lua) is always the active,
-- scroll-focused candidate; the rest keep their nearest-first order:
--   { "active_index":0, "count":N, "targets": [
--       { "address":hex, "full_name", "short_name",
--         "screen": {"x","y"}, "world": {"x","y","z"} },
--       ...
--   ] }
--
-- Fires on every scroll-select change and whenever the active candidate changes,
-- NOT every frame — cheap to just store the last payload and draw it in on_frame.

print("Initializing uobjecthook_picker_selection_demo.lua")
local _fast = uevr.api_fast
local last_targets = {}
local event_count = 0

local function dupe_shape(shape)
	local count = shape.GetNumChildrenComponents and shape:GetNumChildrenComponents() or 0
	if count > 0 then
		local actor = shape:GetOwner()
		local dupe = _fast.add_component(actor, shape:get_class(), false)
		local prop = shape:get_class():get_child_properties()
		while prop ~= nil do
			pcall(function()
				local pname = prop:get_fname():to_string()
				local value = shape:get_property(pname)
				if value then
					dupe:set_property(pname, value)
				end
			end)
		end
	end
end

local function draw_debug_shape(shape, color)
	if not UEVR_UObjectHook.exists(shape) then return end
    shape:SetHiddenInGame(false, false)
    shape:SetVisibility(true, true)
    shape:SetRenderInMainPass(true)
    shape:SetRenderCustomDepth(true)
    shape:SetCustomDepthStencilValue(255)
    shape:SetCustomDepthStencilWriteMask(1)
    for _,v in ipairs({"ShapeColor", "BrushColor", "ArrowColor"}) do
        if shape:get_property(v) then
            shape:set_property(v, color)
        end
    end
end

local target_classes = {
	"LightComponent",
	"SkeletalMeshComponent",
	"ShapeComponent"
}

uevr.sdk.callbacks.on_lua_event(function(name, data)
	if name ~= "uobjecthook_picker_selection" then
		return
	end

	event_count = event_count + 1

	local ok, decoded = pcall(json.load_string, data)
	if not ok or decoded == nil then
		print("[picker_selection_demo] failed to parse payload: " .. tostring(decoded))
		return
	end

	last_targets = decoded.targets or {}

	local active = last_targets[1]
	if active ~= nil then
		print(
			string.format(
				"[picker_selection_demo] #%d active=%s (%s) candidates=%d",
				event_count,
				active.short_name or "?",
				active.full_name or "?",
				#last_targets
			)
		)
	else
		print(string.format("[picker_selection_demo] #%d no candidates in range", event_count))
	end
end)

-- Live overlay: draw a small panel listing every candidate this frame, plus a
-- world-space label glued to the active one so you can see it track in VR too.
uevr.sdk.callbacks.on_frame(function()
	if uevr.params.functions.is_drawing_ui() then
		imgui.begin_window("Picker Selection Demo")
		imgui.text(string.format("Events received: %d", event_count))
		imgui.text(string.format("Candidates: %d", #last_targets))
		imgui.separator()

		for i, t in ipairs(last_targets) do
			local prefix = (i == 1) and "[ACTIVE] " or "  "
			imgui.text(prefix .. (t.short_name or "?"))
			imgui.text("  " .. (t.full_name or "?"))
			imgui.text(string.format("  addr=%s", t.address or "?"))
		end

		imgui.end_window()
	end
end)

uevr.sdk.callbacks.on_frame(function()

	local active = last_targets[1]
	if active == nil or active.screen == nil then
		return
	end
	if uevr.params.functions.is_drawing_ui() then
		local dl = imgui.get_background_draw_list()
		if dl == nil then
			return
		end

		local x, y = active.screen.x, active.screen.y
		dl:add_circle({ x, y }, 14.0, 0xFF00FF00, 20, 2.0) -- opaque green ring (ABGR)
		dl:add_text({ x + 16, y - 8 }, 0xFFFFFFFF, active.short_name or "?")
	end
end)

uevr.sdk.callbacks.on_script_reset(function()
	print("Resetting uobjecthook_picker_selection_demo.lua")
end)