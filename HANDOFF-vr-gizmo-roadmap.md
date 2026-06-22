# Handoff — VR gizmo / overlay projection roadmap

Written 2026-06-17 for the next agent. Branch `luavrlib`. Everything below is **uncommitted** working-tree state. I (the previous agent) cannot test VR (no headset; games pause present/tick when unfocused, so MCP can't drive them) — anything marked "unverified" needs the user to confirm in-headset.

## Ground rules / landmines (read first)

- **Contended files — do NOT clobber.** `src/mods/bindings/SDKFast.cpp`, `src/mods/bindings/ImGui.cpp`, `src/mods/bindings/FS.cpp` are modified+uncommitted by the user's *parallel* Lua-dev instance. Editing them risks destroying that work. The established safe pattern for SDKFast is **append-only** (add new functions + new `t["name"] = ...` lines; never rewrite existing ones). If in doubt, ask the user to commit their parallel work first.
- **Build:** `cmake --build build --config RelWithDebInfo --target uevr -- /m` (or `build.ps1`). The full build (no `--target uevr`) fails on the unrelated `plugin_renderlib` example (stale vendored `examples/renderlib/imgui/imgui.h` vs the imgui 1.92.7 submodule bump, commit `59ce224`) — that's pre-existing, ignore it; only the `uevr` target matters.
- **Deploy:** `%APPDATA%\UnrealVRMod\UEVR\UEVRBackend.dll` is a **symlink** → `build\bin\uevr\UEVRBackend.dll` (restored 2026-06-17). Rebuilds auto-apply. It has been found clobbered-by-a-stale-copy before — after building, verify `(Get-Item $link -Force).LinkType -eq 'SymbolicLink'` and that its hash matches the build output before asking the user to test.
- **DLL lock:** a running UE game or `CrashReportClient.exe` holds the DLL → `LNK1168`. Close them before linking.
- `VR::get()` returns `std::shared_ptr<VR>`, **not** a raw pointer (`auto vr = VR::get();`, not `auto*`).

## DONE this session (baseline you're building on)

1. **Gizmo "offset when UI open" fix (#1).** Root cause: gizmo projects with `UGameplayStatics::ProjectWorldToScreen` (flat game camera) into the imgui background drawlist, which is composited onto the VR overlay quad. UI **closed** = slate overlay placement → pixels are world-aligned. Opening the UI swaps to the framework overlay (different distance/size/`scale_factor`=rt_width/1920/curvature) → same pixels drift.
   - Fix = `vrmod::OverlayComponent::transform_world_aligned_to_overlay(ImVec2)` — `src/mods/vr/OverlayComponent.cpp` (defined just above `update_overlay_openvr()`), declared public in `OverlayComponent.hpp`. Center-scales by `S = (slate_size·fw_dist)/((slate_dist-0.01)·fw_size·scale_factor)`; identity when `!is_drawing_ui()`.
   - Used in the gizmo `project` lambda in `UObjectHook::draw_component_gizmos()` (`src/mods/UObjectHook.cpp`, grep `transform_world_aligned_to_overlay`) gated on `vr->is_hmd_active()`.
2. **Live tuning params (C).** Two sliders in `OverlayComponent.hpp` (grep `m_framework_gizmo_correction`, `m_framework_gizmo_scale`), added to `m_options` so they show in the VR overlay settings UI and persist:
   - `UI_Framework_Gizmo_Correction` (0..2, default 1): strength on the `(S-1)` deviation. 0 = no remap (old behavior), 1 = full, >1 = over-correct.
   - `UI_Framework_Gizmo_Scale` (0.25..4, default 1): raw uniform fudge multiplier about center.
   - Applied as `S_eff = (1 + (S-1)*strength) * extra` in `transform_world_aligned_to_overlay`.
   - **User instruction:** in-headset, set curvature 0, then adjust `_Correction` until the gizmo sits on the object; use `_Scale` only if the whole thing is mis-scaled. If `_Correction`≈0 works best, my `S` model is wrong/inverted — report the value.
3. Builds clean (`uevr` target, exit 0), DLL deployed via symlink. **Unverified in-headset.**
4. (Earlier, separate) Mortal Shell 2 crash: minimal null-guard at `FFakeStereoRenderingHook.cpp:5284`/`5310`; full fix (locked `m_runtime` accessor) still open — see auto-memory `vr_runtime_race_2026_06_16.md`.

---

## TODO

### TODO #2 — camera_attach not reflected in gizmo projection
**Goal:** when the user attaches the UEVR camera to an object/component, the gizmo should project from the *shifted* viewpoint.

**Why it's broken:** the gizmo projects via `ugs->world_to_screen(pc, ...)` using the game `PlayerController`/`PlayerCameraManager` POV. Camera-attach shifts the *rendered* eye but (apparently) not that POV. See the attach math: `UObjectHook.cpp` `m_camera_attach` handling, **lines ~2529-2561** (`adjusted_loc = location - (quat_converter * (rotation_glm_quat * ue4_to_glm(m_camera_attach.offset)))`). Struct/serialize at ~3246, ~3490.

**Investigation needed (first):** confirm whether UEVR writes the attach offset into the `PlayerCameraManager` POV (then `ProjectWorldToScreen` would already see it and this is a non-issue) or only into the stereo view-offset path. Check `FFakeStereoRenderingHook` / `IXRTrackingSystemHook` for where the camera POV vs the stereo offset is applied. If only stereo: in `draw_component_gizmos`' `project` lambda, offset the world point by `+m_camera_attach.offset` (rotated into world space the same way 2542/2561 do it) before `world_to_screen`, OR project relative to the attached object. Needs headset verification.

### TODO #3 — expose the VR-aware world→screen to Lua
**Goal (user, hard requirement):** "whatever math projects the world location to screen must be accessible to any imgui calls from the Lua side."

**Where:** `src/mods/bindings/SDKFast.cpp` (CONTENDED — append-only, or wait for the user to commit). Existing pieces to reuse:
- `static glm::vec2 world_to_screen(uevr::API::UObject* player_controller, const glm::vec3& world_location)` — **line 505**, bound `t["world_to_screen"]` at **891**.
- Registration: `bindings::open_sdk_fast(sol::state_view& lua)` at **852**; table surfaced as `lua["uevr"]["api_fast"] = t` at **928**.

**Sketch (append a new fn, don't edit the old one):**
```cpp
// VR-aware: same flat projection, then remapped onto the framework overlay quad so it
// stays world-aligned while the UEVR UI is open (matches the built-in gizmo). Falls back
// to the flat result when no HMD is active.
static glm::vec2 world_to_screen_overlay(uevr::API::UObject* player_controller, const glm::vec3& world_location) {
    auto sp = world_to_screen(player_controller, world_location); // reuse existing (line 505)
    auto vr = VR::get();
    if (vr != nullptr && vr->is_hmd_active()) {
        const ImVec2 r = vr->get_overlay_component().transform_world_aligned_to_overlay(ImVec2{sp.x, sp.y});
        sp.x = r.x; sp.y = r.y;
    }
    return sp;
}
// ... then in open_sdk_fast, near line 891:
t["world_to_screen_overlay"] = &world_to_screen_overlay;
```
Needs `#include "VR.hpp"` (pulls in OverlayComponent transitively) and imgui in SDKFast.cpp — check they're present. `transform_world_aligned_to_overlay` is already public.

### TODO B — Lua example for the new function
**Goal:** a runnable example script demonstrating `uevr.api_fast.world_to_screen_overlay` + imgui draw. **Blocked on #3.** Example scripts live in the scripts root (autoloads; see auto-memory `uevr_lua_script_loading_and_test_harness.md`). Watch the Lua 5.4 line-continuation trap (a `(`-start line after a call glues into one expression — prefix such lines with `;`). Draft to drop in once #3 lands:
```lua
-- world_anchor_demo.lua — draws a label that sticks to a world point, aligned in VR.
local target = nil  -- set to an actor/component world pos source
uevr.sdk.callbacks.on_draw_ui(function()
    local pc = uevr.api_fast.get_player_controller(0)
    if pc == nil or target == nil then return end
    local p = uevr.api_fast.world_to_screen_overlay(pc, target)  -- VR-aligned
    if p == nil then return end
    local dl = imgui.get_background_draw_list and imgui.get_background_draw_list() or nil
    if dl then dl:add_circle_filled({p.x, p.y}, 6, 0xFF00FFFF) end
end)
```
(Verify the exact imgui binding names against the parallel instance's `ImGui.cpp` — `get_background_draw_list`/`add_circle_filled` naming may differ.)

### TODO A — intuitive gizmo-adding: spawner + click-select + auto-add (unstarted, biggest)
**Problem (user):** adding gizmos via the per-component "Show gizmo" checkbox is unintuitive. Want easier ways to put objects on the gizmo list (`m_gizmo_components`). User's proposed directions (any one reduces the need for the others):
- **Lua API to add to the gizmo list** — expose UObjectHook so a script can push/remove components on `m_gizmo_components` (and read back). This is the explicit "more UObjectHook exposure to Lua" ask. Lives alongside #3. Sketch: a `uevr.uobjecthook` table with `add_gizmo(component)`, `remove_gizmo(component)`, `list_gizmos()`, plus the `world_to_screen_overlay` from #3. NOTE the contended-file rule (bindings live in `SDKFast.cpp`/`ImGui.cpp`/`ScriptContext.cpp`). UObjectHook is a singleton (`UObjectHook::get()`); `m_gizmo_components` is `std::unordered_set<sdk::USceneComponent*>` — guard with `m_mutex` and validate via `this->exists(comp)` before insert (the gizmo draw already does).
- **Mouse-click select** (see spawner/click-select below) — click an actor/component to add its gizmo. Makes the Lua-add and checkbox mostly unnecessary.
- **Auto-add gizmo** for components that already have (a) **overlappers attached** or (b) a **motion-controller attachment set to "adjust mode"**. **[PARTIAL — DONE for the MC-adjust-mode half, 2026-06-17]:** `draw_component_gizmos` now builds a transient `draw_comps = m_gizmo_components ∪ {MC-attached comps where state->adjusting}` when the new `m_auto_gizmo_on_adjust` toggle is on ("Auto-gizmo on MC adjust (VR)" checkbox in the gizmo Settings popup). Default OFF (zero regression). Built+deployed, NOT headset-verified. STILL TODO here: the **overlappers** half (auto-add for comps with overlappers attached — needs the overlapper registration point), and confirming the adjust-mode gizmos are draggable/useful in VR. The MC-attach/adjust-mode state is in `m_motion_controller_attached_components` / `MotionControllerState` (UObjectHook.cpp ~1998-2034, ~2484, ~2593; "adjust" path ~2960-3019). Hook the point where an MC attachment enters adjust mode (or an overlapper is registered) to also `m_gizmo_components.insert(comp)`. This is the most "intuitive" per the user — gizmos appear exactly on the things you're already manipulating.

**Goal:** UObjectHook UI to (a) spawn an object/actor by class, (b) click-select an actor/component in the world to drive the gizmo / camera-attach / inspector.

**Spawner — reuse existing SDK wrappers (`SDKFast.cpp`):**
- `spawn_object(class_name, outer)` — **line 486** (`UGameplayStatics::spawn_object`).
- `spawn_actor(class_name, location)` — **line 495** (`UGameplayStatics::spawn_actor`, world auto-fetched).
- `find_class(short_name)` — **line 381** (short-name → UClass cache).
- C++ side, the same `sdk::UGameplayStatics::get()->spawn_actor(world, cls, location)` can be called directly from UObjectHook (no Lua needed). UI: add a "Spawn" panel near the class browser. The class-browser / "Objects by class" UI and the selection set live in `UObjectHook.cpp` — grep `m_gizmo_components`, `"Show gizmo"` (~5937, ~6616), `m_show_class_browser`, `Objects by class`. Spawn location default = in front of the player camera (PlayerCameraManager POV + forward*N), or at the attached camera.
- After spawn, auto-select: insert the new component into `m_gizmo_components` and/or set `m_camera_attach.object`.

**Click selection — two paths:**
1. **Desktop/mouse (flat):** on click, `UGameplayStatics::DeprojectScreenToWorld(pc, mouse_xy, &world_origin, &world_dir)` then `UKismetSystemLibrary::LineTraceSingle(...)` (SDK has UGameplayStatics + UKismetSystemLibrary only — no KismetMathLibrary). Hit actor → select. Check the SDK headers for these functions; add thin wrappers like the existing spawn ones if missing.
2. **VR (controller laser):** mirror the existing motion-controller picking — `UObjectHook.cpp` ~**2818** does an overlap (`get_or_add_motion_controller_state(overlap)`) from controller pose; reuse `vr->get_position/get_rotation(controller_index)` for a ray and trace. The overlay already computes a controller→quad intersection for mouse emulation (`OverlayComponent.cpp` ~775-790) — that's for UI, but the same controller ray can trace the world for picking.

**Gotchas:** spawning/tracing must run on the game thread (these MCP/game-thread calls no-op when the game is unfocused). Verify class names resolve via `find_class` (short-name). All of A needs in-game + in-headset verification.

---

### TODO — Save Property regression (partial; "keep what works")
**Finding (investigated 2026-06-17, NOT actually fully lost):** the "Save Property" right-click is **intact for scalar properties** — `UObjectHook::ui_handle_properties` (UObjectHook.cpp **7132**) renders scalars at ~7385-7451 and calls the `display_context` lambda (**7277**) which shows the `"Save Property"` button (**7373**) backed by `save_logic` → `PersistentProperties::save_to_file` (8351) and the apply loop (3539-3990). Right-clicking a plain float/int/byte field still works.

**The gap:** struct properties (Vector / Rotator / Transform — the common things you'd save) now render through the compact `ui_try_known_struct` path (called at **7625**, defined **8083**) which has **no context menu**, so Save Property is unreachable *for structs*. That refactor (commit `dfe6392`, "compact math-struct display") is what made it feel lost.

**Restore approach (do NOT blind-extend serialization):** the apply loop at 3539-3990 already logs `"Unsupported persistent property type"` for many types — the persistence backend is fragile (user: "isn't that good at handling serialization anyway"). So:
1. Cheapest safe win: re-expose Save on the struct path by giving the `ui_try_known_struct` row a `BeginPopupContextItem` that calls a struct-aware `save_logic` (memcpy `definition` struct size bytes instead of `sizeof(value)` — `PersistentProperties::PropertyState::data` buffer size must be checked/enlarged in UObjectHook.hpp ~477-508).
2. **Only after** verifying the round-trip in a live game (save → reload → property re-applied). Without that, a Save that writes a file but fails to re-apply is worse than today. Needs game, not headset.
3. Minimum: preserve the working scalar path (don't let further inspector refactors drop the `display_context` calls).

## Quick map of key symbols
| What | Where |
|---|---|
| Gizmo draw + `project` lambda | `UObjectHook::draw_component_gizmos()` — `src/mods/UObjectHook.cpp` (grep the fn name) |
| Overlay remap fn + tuning sliders | `src/mods/vr/OverlayComponent.cpp` (`transform_world_aligned_to_overlay`), `.hpp` (`m_framework_gizmo_*`) |
| Framework vs slate overlay placement | `OverlayComponent.cpp` `update_overlay_openvr()` ~lines 730-757 (OpenVR); OpenXR path = `OverlayComponent::OpenXR::generate_framework_ui_quad` / `generate_slate_*` |
| camera_attach | `UObjectHook.cpp` ~2529-2561, ~3246, ~3490 |
| Motion-controller picking | `UObjectHook.cpp` ~2818 |
| Gizmo list (selection set) | `m_gizmo_components` (UObjectHook.hpp); insert/erase via "Show gizmo" ~5937/6616 |
| MC attachments / adjust mode | `m_motion_controller_attached_components`, `MotionControllerState` — UObjectHook.cpp ~1998-2034, ~2484, adjust path ~2960-3019 |
| Property inspector + Save Property | `ui_handle_properties` 7132; `display_context`/"Save Property" 7277/7373; struct path `ui_try_known_struct` 7625/8083; persist apply 3539-3990, `save_to_file` 8351 |
| Lua api_fast bindings | `src/mods/bindings/SDKFast.cpp` (`open_sdk_fast` 852, `uevr.api_fast` 928) — CONTENDED |
| VR accessors | `VR.hpp`: `get_overlay_component()` 287, `is_hmd_active()` 265, `get_current_projection_matrix`/`get_current_eye_transform` 209-212 |

Relevant auto-memories: `vr_gizmo_overlay_remap_2026_06_17.md`, `vr_runtime_race_2026_06_16.md`, `uevr_dll_injection_path.md`, `uevr_mcp_and_build_gotchas.md`, `uevr_lua_script_loading_and_test_harness.md`.
