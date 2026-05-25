# User Lua scripts — luavrlib branch compatibility audit

_Date: 2026-05-25 (against `Scripts/` snapshot at the same date)_

This is a short summary of the user's `C:\Users\lbatv\AppData\Roaming\UnrealVRMod\UEVR\Scripts\` folder against the new Lua API on this branch. The audit treats the **post-March/April-2026** scripts as canonical (per the user's instruction) and ignores older files unless they are required by a newer one.

## Bottom line

Almost every recent script keeps working unchanged. The luavrlib changes are mostly **additive** (multistate workers, `uevr.api_fast.*`, more drag-drop helpers) or **transparent fixes** (TArray of FName / FString / StructProperty / out-params now produce sane Lua values where before they were nil or garbage). Two scripts in `Scripts\Tests\` were broken against the old API and have been migrated:

| File | Status | Notes |
|------|--------|-------|
| `Scripts\Tests\tarrayfname.lua` | rewritten | Old version had nested-`end` typo, no Mesh/RootComponent fallback, no error isolation. Now caches both FName userdata and narrow strings; on_frame just draws cached values. |
| `Scripts\Tests\grok.lua` | rewritten | Removed `require("API_Mod_Lua")` and `Kismet` deps. Replaced `imgui.text(t.rotation)` with explicit tostring. Fixed `Matrix4x4f.indentity()` typo. Wrapped every panel in `pcall` so one panel error no longer kills the whole script. Removed ~200 lines of commented dead code. |
| `Scripts\Tests\new_api_panels.lua` | NEW | Comprehensive interactive checklist covering Vector/Quat/Matrix/Transform, TArray<FName>, TArray<UObject*>, StructObject read/write, worker thread benchmark, function caller, fast-path transform benchmark. |

## Scripts checked

All files modified on/after 2026-04-01 in `Scripts/`. Spot-checked, not line-by-line.

| File | Date | Result |
|------|------|--------|
| `API_Main.lua` | 2026-05-22 | Compatible. Heavy use of `find_uobject`, `get_objects_matching`, `get_full_name`, `get_fname`, `pcall` — all unchanged on this branch. |
| `APIUE.lua` | 2026-05-11 | Compatible. Uses the standard reflection chain for `_uobject:get_transform/get_location/get_rotation/...`. Now ALSO has a faster alternative — see "Fast-path migration" below. |
| `API_Mod_GLM.lua` | 2026-05-02 | Compatible. The Vector/Quat/Matrix metamethods are a strict superset of what API_Mod_GLM relies on (it doesn't use the new `to_vec3d` / `to_quatf` conversions, but it isn't affected by them either). |
| `API_Mod_ImGui.lua` | 2026-04-26 | Compatible. The deduplicated `bindings::open_imgui()` keeps the original 207 entries. |
| `API_Mod_VR.lua` | 2026-04-21 | Compatible (no API changes in VR namespace on this branch). |
| `API_Mod_Lua.lua` | 2026-05-01 | Compatible. The `uevr.lua` table now also exposes `add_script_panel` directly; the old `uevr.lua.add_script_panel` path is the same code. |
| `API_Mod_Draw.lua` | 2026-04-14 | Compatible (ImDrawList APIs unchanged, path_arc/bezier/etc are additive). |
| `Main.lua` | 2026-05-08 | Compatible (loads the modules above). |
| `BoneUtil.lua` | 2026-05-25 | Compatible. Uses `GetSocketTransform`, `K2_GetComponentTransform` — both work through standard reflection. |
| `CameraManager.lua` | 2026-05-18 | Compatible. |
| `Hit.lua` | 2026-05-18 | Compatible — uses `process_event`/`call_function` on `LineTraceSingleByChannel` etc. |
| `Inputs.lua` | 2026-05-04 | Compatible. |
| `Colors.lua` | 2026-05-04 | Compatible (pure Lua tables). |
| `NewEnvdump.lua` | 2026-05-18 | Compatible (no UEVR API usage). |
| `Panels.lua` | 2026-04-21 | Compatible (panel registration). |
| `ImGui.lua` | 2026-04-29 | Compatible. |
| `inspect.lua` | 2026-04-21 | Pure Lua, unaffected. |
| `class.lua` | 2026-04-21 | Pure Lua, unaffected. |
| `Examples/*.lua` | various | Spot-checked half a dozen — `Pong.lua`, `MaterialMenu.lua`, `MeshPanel.lua`, `Path.lua`, `ScriptPanels.lua`. None use removed APIs. |

## Fast-path migration (optional, not required)

`uevr.api_fast.*` (added in commit `6399b98`) is a strict-faster alternative to the `_uobject:get_location()` / `get_rotation()` / `get_transform()` chain in APIUE.lua. It collapses the per-call `if/elseif` chain in Lua and the per-call `__index` lookup into a single SDK call that holds the underlying UFunction in a function-local `static`. For hot loops (e.g. polling transforms every frame for >50 actors), replacing:

```lua
local loc = actor:K2_GetActorLocation()
```

with:

```lua
local loc = uevr.api_fast.get_actor_location(actor)
```

is roughly 3-5× faster per call on synthetic benchmarks (see `Tests/new_api_panels.lua` § "[Benchmark] transform fast-path"). It does **not** avoid the underlying `process_event` — there is always one — only the Lua-side dispatch around it. For a one-off read per frame it's not worth changing.

Both call paths produce the same Vector3f userdata, so the rest of the script (`.x`, `.y`, `.z` access, arithmetic) is unchanged.

## Pre-March/April files

The user explicitly asked to ignore pre-March scripts unless referenced. Spot-checked the index in `Main.lua` and the `require(...)` calls in `API_Main.lua` / `APIUE.lua` — no old-dated files are required transitively. Subfolders `_Game_AssetDesign/`, `New folder*/`, `WIP/`, `TEMP/`, `backup/`, `disable/`, `disable2/`, `disable4/`, `Testing/`, `Loose/`, `Tarray/`, `LIB/`, `UI/`, `Developer/`, `Example folder/`, `data/` were not crawled — they appear to be user scratch space.
