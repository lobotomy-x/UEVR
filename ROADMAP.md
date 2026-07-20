# UEVR luavrlib — unified roadmap

Single source of truth for the UObjectHook / gizmo / VR work. Consolidates asks from this and prior sessions. Deep VR-projection details live in `HANDOFF-vr-gizmo-roadmap.md`; this file is the task ledger.

Legend: ✅ done+built (unless noted, NOT in-headset/in-game verified) · 🧩 STUB wired (builds, empty body for offload) · ⏳ TODO (not started) · 🟡 partial

Build: `cmake --build build --config RelWithDebInfo --target uevr -- /m` (ignore the unrelated `plugin_renderlib` example failure). Deploy is the symlink at `%APPDATA%\UnrealVRMod\UEVR\UEVRBackend.dll` → `build\bin\uevr\UEVRBackend.dll`. Kill `CrashReportClient` if you hit LNK1168.

---

## LATEST STATUS — 2026-07-07 (autonomous scheduled-run continuation; UNCOMMITTED working-tree changes; built exit 0, DLL deployed via symlink, hash verified; NOT in-game/headset verified)

- ✅ **#3 Lua exposure, first half — `uevr.api_fast.world_to_screen_overlay`** (`src/mods/bindings/SDKFast.cpp`, APPEND-ONLY per the contended-file rule; left UNCOMMITTED because the file carries the parallel instance's uncommitted work — do not fold into an unrelated commit). Reuses `world_to_screen` then remaps via `OverlayComponent::transform_world_aligned_to_overlay` when `vr->is_hmd_active()`; identity when flat. Added `#include "../VR.hpp"`. REMAINING from #3: the UObjectHook gizmo-list API (`add_gizmo`/`remove_gizmo`/`list_gizmos`).
- ✅ **TODO B — example script** `lua-api/examples/world_anchor_demo.lua` (new file): anchors a marker+label to the local pawn's world location via `world_to_screen_overlay` on `on_frame`, drawn with `imgui.get_background_draw_list()` (verified binding names `add_circle_filled`/`add_text` against `ImGui.cpp`). Needs in-game (ideally in-headset, UI open) verification.
- NOTE: the hourly "rerun-limited-sessions" scheduled task had produced ~366 stub sessions while Fable 5 was unavailable/limited; a concurrent run of it was active during this pass (blocked on a permission prompt), so work was kept to append-only + new files to avoid collisions.

## Previous status — 2026-06-24 (autonomous flat/desktop pass, branch `auto-roadmap-2026-06-24` off `luavrlib`; each item is its own atomic, revertible commit; all build clean; NOT in-game/headset verified)

Four non-headset-gated gizmo items implemented, built (`cmake --build build --config RelWithDebInfo --target uevr`, exit 0 each) and committed individually so any one can be reverted in isolation:

- ✅ **D1 — plane handle grabbable anywhere inside the quad** (commit `8a7b906`). Added a `point_in_quad` helper; the translate plane hit-test now accepts a click anywhere inside the plane square (ranked by distance to the quad centroid so a single-axis line running along an edge still wins when closer), instead of only near the outer corner. `draw_component_gizmos` hit-test, `UObjectHook.cpp`.
- ✅ **D2 — show all 3 gizmo types at once** (commit `168cf37`). New toggle "Show all 3 gizmo types (offset)" + spacing slider (persisted via `UObjectHook_GizmoShowAllModes`). Draws the two inactive modes (move arrows / rotate rings / scale boxes) as compact, non-interactive reference glyphs offset in screen space from the live gizmo. Purely additive draw built from already-projected axis tips — can't affect hit-testing/drag.
- ✅ **D4 — Recenter to camera button** (commit `bfe05de`). On the pinned "Selected:" panel: deprojects the screen centre to a camera ray (`UGameplayStatics::screen_to_world`) and places the object at POV + forward × distance (new `m_recenter_distance`, default 150 cm, with a drag field). Engine calls deferred to the game thread via `GameThreadWorker`.
- 🟡 **#2 — highlight the adjusted axis (flat half done)** (commit `34f70af`). Dragging an X/Y/Z field of the Location/Rotation/Scale `DragFloat3` now lights up the matching gizmo handle (detected by diffing the slider value, expires ~2 frames after the last change). VR thumbstick half (E1/E2) still open.

### Previous status — 2026-06-23 (all committed + pushed to fork `lobotomy-x/UEVR` branch `luavrlib`, tip `2cb8e2c`; built+deployed; NOT in-headset-verified)

This supersedes the stale "Save Property" / "#1 click-select" / texture entries below. Each change-set went through adversarial multi-agent review; confirmed bugs were fixed and re-verified.

- ✅ **Save Property — FIXED & WORKS** (was the "still broken" item). Three root causes fixed: (1) a popup hijack (`<name>EditFlags` popup stole the right-click) hid the Save button — consolidated to one menu per row gated on `row_ctx_drawn`; (2) the hard `has_valid_base()` gate — now `resolve_save_target()` falls back to `try_get_path` then a full-name locator; (3) known-struct save bound to the trailing Copy button — now `OpenPopupOnItemClick` on the value row. Also added UInt64/Int64 reapply + "Persistent Level" base fix.
- ✅ **Stable object-locator model** — arbitrary objects (no allowed-base path, e.g. click-selected world actors) are now saveable: `PersistentProperties.object_locator` (= `get_full_name()`), `resolve_persistent_target()` re-resolves via `sdk::find_uobject` with a per-bucket miss-cooldown (re-probes ~every 20 ticks; on-click dedup bypasses it). Class is embedded in the locator ⇒ no wrong-offset write. Limitation: runtime-spawned objects with session-varying name numbers won't re-match.
- ✅ **#1 click-to-select gizmo targets** — one-shot "Pick gizmo target" button (auto-disarms after a hit, Esc cancels, optional sticky). Deprojects the cursor (`screen_to_world`), picks the front-most scene component in a cone, adds to `m_gizmo_components`. Suppresses the gizmo-axis grab on the pick frame; yields VR drag-scroll while picking.
- ✅ **Lua `uevr.reset_scripts()`** — deferred-safe (flag drained at top of `LuaLoader::on_frame`).
- ✅ **Numbered-component reapply** — `StatePath::resolve` now exact-matches the saved numbered sibling first (de-numbered prefix as fallback), so a save no longer reapplies onto the wrong same-base-name instance.
- ✅ **Global drag-to-scroll** — matches the class browser (`drag_scroll_current_window`): MIDDLE mouse flat / VR-left (yields to gizmo/picker), 2.5x/1.5x, ResizeAll cursor. + window width cap (~900px) for readable columns.
- ✅ **Texture preview D3D11 crash — FIXED** (was crashing). The flat-mode FRHITexture2D vtable "bootstrap" was wrong (scanned for a `d3d11.dll` vtable, but UE's FD3D11Texture2D vtable is in the game module) → corrupted virtual calls → crash. REMOVED it; flat mode now shows "enter VR to capture vtable", works in VR. **NOTE: the "🟡 Texture preview" + "D3D11 texture preview bootstrap" entries below are OBSOLETE** — there is no offset/bootstrap to fill in; the feature is VR-only until a proper flat-mode RHI hook exists.

Still open (mostly headset-gated — do NOT implement blind): **#2** gizmo highlight adjusted axis (thumbstick/slider), **D2** show 3 gizmos offset, **recenter** button, **E1/E2/E3** VR stick-adjust, **camera_attach #2**, **#3 OpenXR overlay-missing** (needs a failing game's log), Mortal Shell 2 `m_runtime` UAF full fix.

---

## ✅ Done this session (in the deployed DLL, uncommitted)

| Feature | Where | Verified |
|---|---|---|
| ✅ D3D12 stale-imgui.ini MV crash fix (`sanitize_imgui_ini`) | `Framework.cpp` (helper after `g_framework`; calls before both `IniFilename=`) | **USER-CONFIRMED in-game** |
| ✅ Property type-filter + group by base class / type | `UObjectHook.cpp` `ui_handle_properties` (~7151) | **USER-CONFIRMED in-game** |
| ✅ Copy/paste struct widget (Vector/Rotator/Quat/Transform) | `UObjectHook.cpp` `ui_struct_clipboard` (above `ui_try_known_struct`), called in the math + Transform branches | built, not verified |
| ✅ Auto-gizmo on MC adjust mode (toggle, default off) | `UObjectHook.cpp` `draw_component_gizmos` `draw_comps`; `m_auto_gizmo_on_adjust` | built, VR-unverified |
| ✅ VR gizmo overlay remap + 2 live tuning sliders | `OverlayComponent` `transform_world_aligned_to_overlay`; `m_framework_gizmo_correction/_scale` | built, VR-unverified |
| ✅ Mortal Shell 2 `m_runtime` null-guard (partial) | `FFakeStereoRenderingHook.cpp:5284/5310` | built; UAF path still open |

## 🟡 Implemented, pending game-specific data

- 🟡 **Texture / Texture2D preview as ImGui::Image** — `UObjectHook::draw_texture_preview()` is now **fully implemented for D3D11**: walks UTexture2D→FTextureResource→FD3D11Texture2D→ID3D11Texture2D, creates+caches an SRV, aspect-fits and draws `ImGui::Image`. Fully guarded (IsBadReadPtr + try/catch) so a wrong offset bails safely instead of crashing. **The only fill-ins are 3 engine-version offsets** (`RESOURCE_OFF` / `TEXTURE_RHI_OFF` / `D3D11_TEX_OFF`, default 0 = shows "set the 3 offsets"); find them via `uevr_read_memory` / RTTI on a known texture (instructions in the function header). D3D12 path shows "only D3D11 implemented". Gated behind `m_show_texture_previews` ("tex preview" checkbox, default OFF).

## Session 2026-06-21 — in-game bug fixes + add/select-objects epic

DONE (built+deployed):
- ✅ **D3D12 multiviewport crash on moving the overlay** — gate (`Framework.cpp` ~1944) now excludes D3D12 from `ViewportsEnable` (was flat-D3D12-only-MV → popping the overlay out spawned a secondary D3D12 swapchain → crash). MV now off on both renderers.
- ✅ **D3D11 texture preview "didn't work"** — ROOT CAUSE: `FRHITexture2D` vtable only captured in UEVR's VR render hook (`FFakeStereoRenderingHook.cpp:6945`), so flat D3D11 → "vtable is null" → every calibration fails (confirmed in live log). FIX: `draw_texture_preview` now bootstraps the vtable from the inspected texture's own resource chain (finds the pointer whose vtable is in d3d11/d3d12.dll), gated behind the experimental toggle, fully guarded. VERIFY by re-inspecting a texture — log shows "bootstrapped FRHITexture2D vtable" + "Found UTexture::PrivateResource offset".
- ✅ **Add Component: short-name + drag-drop + common buttons** (#3/#4) — `ui_handle_*` Add-Component block refactored to one `queue_add`; field resolves full path OR short name (retried under `/Script/Engine.`); accepts a **UClass dropped from the class browser** (`accept_class_drop`, payload "UEVR_UClass"); row of quick buttons (StaticMesh/PointLight/SpotLight/Sphere/Box/Arrow/SceneComponent).

- ✅ **Lua callbacks: on_pawn_changed / on_view_target_changed / on_level_changed** (built+deployed). Implemented in `lua-api/lib/src/ScriptContext.cpp` (the clean parallel file, compiles into `luavrlib.lib` → statically linked into UEVRBackend, so the normal symlink deploy covers it). C++-side polling in `on_pre_engine_tick`: reads `get_local_pawn(0)`, PlayerCameraManager→ViewTarget.Target, and PlayerController->get_outer() (persistent level); fires the matching callbacks only on change (the "C++ callback for speed" the user asked for — no per-frame Lua polling). Lua: `uevr.sdk.callbacks.on_pawn_changed(function(pawn) ... end)` etc. NOT in-game-verified (needs a Lua script).
- ⏳ **Save Property STILL not working — backend blocker pinpointed.** `PropertyState::data` (UObjectHook.hpp:497) is an **8-byte union** (u64/d/f/i/u8/u16/b) so it physically cannot store a struct value (Vector=12B, Transform=48B), and the apply loop (UObjectHook.cpp ~3610) only switches on scalar types. So the transforms the user adjusts can't be saved. FIX (contained to UObjectHook, real backend work): widen `PropertyState::data` to a sized byte buffer (e.g. `uint8_t bytes[64]; uint32_t size;`), update `to_json`/`from_json` (serialize bytes as hex/array — changes save-file format), add a `StructProperty` case to the apply loop (memcpy bytes into the struct), and add the save context-menu to `ui_try_known_struct` rows. Plus the `has_valid_base()` gate still blocks class-browser/address-inspected objects (needs an arbitrary-object re-resolve scheme).

REMAINING in the epic (need design + in-game/headset verify):
- ⏳ **#1 click-based selection of gizmo targets** — click an actor/component in the world to add it to `m_gizmo_components`. Needs DeprojectScreenToWorld + LineTraceSingle (or controller-ray in VR) → hit comp → insert. Verify picking in-game.
- 🟡 **#2 gizmo highlights the adjusted axis when driven by thumbstick OR imgui sliders** — ✅ **flat half DONE (2026-06-24, commit `34f70af`):** `note_driven()` in the transform editor records the dragged sub-axis (value diff) and `draw_component_gizmos`' `driven_hot()` lights the matching handle (expires ~2 frames after the last change). REMAINING: VR thumbstick half via `vr_gizmo_stick_adjust` (E1/E2, headset-gated).

## Session 2026-06-20 feedback + new asks

- ✅ **Inspector filter rework (DONE, built+deployed):** property-type filter is now a **dropdown combo** (distinct types, "All" default), base-class filter is a **second dropdown combo**, plus a **name-search text input**; all three filters combine (AND). **Tree connector lines ON by default** (`style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesFull`). `ui_handle_properties`.
- 👍 Gizmo text overlay (D3) — user: "fantastic, well done". Keep.
- ✅ **#1 Texture preview — DONE via SDK accessors (built+deployed, not in-game-verified).** Rewrote `draw_texture_preview` to use the SDK's self-calibrating `sdk::UTexture::get_texture_rhi()` → `FRHITexture::get_native_resource()` (the same path `D3D11Component.cpp:243` uses for the UE backbuffer) — **no hardcoded offsets**. Gated Texture2D-only (the calibrator `update_render_resource_offset_texture2d` needs a 2D vtable), per-native SRV cache, full guards. D3D12 still shows "only D3D11". Toggle still default OFF.
- ⏳ **#3 OpenXR overlay missing in some games (BUG) — NARROWED 2026-06-20.** The framework overlay only submits if `m_openxr.ever_acquired(FRAMEWORK_UI)` (D3D11Component.cpp:712 / D3D12Component.cpp:670). That flag is set ONLY by the copy at **D3D11Component.cpp:492-494** (D3D12: 392-396), which is gated on `is_right_eye_frame && fw_rt != nullptr && g_framework->is_drawing_anything()`. So in the broken games one of those never holds — prime suspect is **`is_right_eye_frame` never firing** under a single-pass / AFR / mono stereo-submit variant (the slate UI copy at 486 shares the same `is_right_eye_frame` gate, so if BOTH slate+framework are missing, that's the gate). NEXT (needs failing game's `log.txt`): confirm the FRAMEWORK_UI swapchain is created (D3D11Component.cpp:1975) and whether `is_right_eye_frame` ever becomes true. CANDIDATE FIX (only after confirming, it's risky): copy the framework UI once-per-frame outside the right-eye gate when it hasn't been copied yet — but that changes the per-eye submit model, so verify against a working OpenXR game first.
- ⏳ **#4 Save Property "missing" — ROOT-CAUSED 2026-06-20 (by design, not a regression).** The "Save Property" right-click (`display_context`, ~7510) is gated on `previous_path.has_valid_base()` (UObjectHook.hpp:442), which is true ONLY when `m_path[0]` ∈ `s_allowed_bases` = {Acknowledged Pawn, Player Controller, Camera Manager, Persistent Level, World} (hpp:244). So save only appears when you reached the property by navigating from one of those 5 roots (persistence re-resolves the object by walking that path next launch). Inspecting via the **class browser / address lookup** → no valid base → "Can't save, did not start from a valid base" → no button. THAT is the "missing" report. Scalar save from a valid base still works; struct save still absent (structs route through `ui_try_known_struct`, no context menu). FIX (real backend work, needs design + in-game verify — NOT safe blind): add an object-addressing scheme that re-resolves arbitrary objects (full path-name / class+predicate) so save works regardless of nav root; then add the struct-bytes save path. This is the "serialization needs improvement" the user means.

## ⏳ TODO — remaining asks (with anchors + suggested stub points)

Each entry says where to add it and, for the offload-friendly ones, the empty-function signature to drop in so it builds first.

### Gizmo (flat / desktop)
- ✅ **D1 — plane handle now grabbable anywhere inside the quad** (DONE 2026-06-24, commit `8a7b906`, built; not in-game-verified). `point_in_quad` helper + centroid-ranked inside-hit in the `draw_component_gizmos` plane hit-test (`m_gizmo_mode == 0`); the corner tip stays grabbable and single-axis lines still win along the quad edges.
- ✅ **D2 — show all 3 gizmo types at once with offset spacing** (DONE 2026-06-24, commit `168cf37`, built; not in-game-verified). Toggle "Show all 3 gizmo types (offset)" + spacing slider, persisted. Inactive modes drawn as compact non-interactive reference glyphs offset in screen space; additive only.
- ✅ **D3 — AddText overlay: actor + scene-component short name + live transform metrics** (DONE 2026-06-19, built+deployed, not in-game-verified). In `draw_component_gizmos` draw loop: per gizmo, drop-shadowed `dl->AddText` at `sc.s_origin + (10,10)` showing `Actor / Component` + `P x y z / R p y r / S x y z` (from `get_owner()->get_fname()`, `get_fname()`, `sc.origin`, `get_world_rotation`, `get_relative_scale`). Additive text, try/catch-guarded, can't affect interaction.
- 🟡 **D4 — offscreen indicator + recenter.** DONE (2026-06-19): edge-clamped arrow + component name pointing toward any gizmo whose origin projects outside the viewport (in `draw_component_gizmos` draw loop; additive, fires only when off-screen). ✅ **Recenter button DONE (2026-06-24, commit `bfe05de`):** "Recenter to camera" on the Selected panel deprojects the screen centre to a camera ray and writes world location = POV + forward × `m_recenter_distance` (game-thread deferred). REMAINING: behind-camera case for the offscreen arrow (project() returns false → needs camera-relative direction, currently just skipped). Original stub note:
  STUB POINT: add
  `void UObjectHook::draw_gizmo_offscreen_indicator(const ImVec2& screen_pos, bool on_screen, sdk::USceneComponent* comp);`
  called from `draw_component_gizmos` when `project()` returns false / point is outside the viewport → draw an arrow clamped to the screen edge pointing toward it. Plus a button "Recenter object" that sets the component's world location in front of the camera (PlayerCameraManager POV + forward) or at the pawn.

### Gizmo (VR) — all need a headset to verify; good stub candidates
- 🧩 **E1 + E2 — thumbstick drives gizmo axis + show driven axis as "clicked".** STUB WIRED (2026-06-19): `UObjectHook::vr_gizmo_stick_adjust(sdk::USceneComponent*)` exists as an empty no-op with a full implementation context-dump above its definition in `UObjectHook.cpp`; a guarded no-op call site is wired in `draw_component_gizmos` (VR-active path, `if (remap_overlay)`). **To implement: open `vr_gizmo_stick_adjust`, follow the numbered steps.** Builds green today; does nothing until filled (needs headset to verify).
- ⏳ **E3 — object VR-attached to a motion controller + adjust enabled → draw gizmo + edit/display the adjustment data.** Overlaps the ✅ auto-gizmo-on-adjust (which already adds the component to the draw set). Remaining: feed the gizmo's edits back into the `MotionControllerState` offset (`location_offset`/`rotation_offset`, `draw_component_gizmos` ~2879) and display those metrics (ties into D3).

### Inspector / config
- 🟡 **B — Config tab + unify grouping.** DONE (2026-06-20): `draw_config()` now has "Gizmo defaults" (mode/thickness/axis-len/local/labels/auto-gizmo) + "Inspector" (texture previews) sections. REMAINING: ~~(a) persist these~~ DONE (2026-06-20: `on_config_save`/`load` write/read the plain members via `cfg.set/get` — survive restarts, no member-type change). (b) ~~unify grouping with functions~~ ALREADY PRESENT: `ui_handle_functions` (7099) has its own "Group by class" checkbox (7117) + name filter since an earlier commit. Both panels group-by-class; only cosmetic diff (functions=checkbox, properties=combo) remains, not worth touching confirmed-good code. **B effectively complete.** Also fixed (2026-06-20): property type/base filter indices now reset when switching objects (were stale statics). Original ask: Properties grouping is in `ui_handle_properties` (function-local statics `s_prop_group_mode`/`s_prop_type_filter`); functions filtering is in `ui_handle_functions`. Promote both to members, render one shared control, and add default-value widgets to the Config tab (find the existing config/settings tab in `UObjectHook::on_draw_ui`).
- ⏳ **Save Property for struct props** (Vector/Transform) — works for scalars; structs route through `ui_try_known_struct` (no context menu). Give that row a `BeginPopupContextItem` calling a struct-aware save. ONLY after verifying the save→reload round-trip in a live game (backend is fragile). Details in `HANDOFF-vr-gizmo-roadmap.md`.

### From prior sessions (still open)
- ⏳ **camera_attach (#2)** — gizmo projects from the unshifted PlayerController POV; fold `m_camera_attach.offset` in. `UObjectHook.cpp` ~2529-2561. Needs headset.
- 🟡 **Lua exposure (#3)** — ✅ `world_to_screen_overlay` DONE 2026-07-07 (appended to `SDKFast.cpp`, built exit 0, uncommitted; example in `lua-api/examples/world_anchor_demo.lua`). REMAINING: UObjectHook gizmo-list API (`add_gizmo`/`remove_gizmo`/`list_gizmos`). Contended file — append-only; see Parallel section.
- ⏳ **Mouse-click select / spawner / overlapper-auto-add** — see `HANDOFF-vr-gizmo-roadmap.md` TODO-A.
- ⏳ **Mortal Shell 2 `m_runtime` use-after-free** — full fix = locked `get_runtime_safe()` accessor. `vr_runtime_race_2026_06_16.md`.

---

## Parallel-instance files (the "merge without adding broken shit" item)

Three files are modified by your parallel Lua-dev instance and were uncommitted: `bindings/ImGui.cpp`, `bindings/SDKFast.cpp`, `bindings/FS.cpp`. **Status: they are already compiled into the current build** (they're in the working tree, so every build above includes them) and the build is green — so they are NOT adding broken-at-compile shit. Breakdown:

- **`ImGui.cpp` (+13, additive, safe):** adds Lua `draw.line / outline_circle / filled_circle / outline_quad / filled_quad` helpers + `lua["draw"]`. Pure additions, no edits to existing bindings. ✅ safe to keep/commit.
- **`SDKFast.cpp` (+49, additive, safe):** new `uevr.api_fast` functions, append-style. ✅ safe to keep/commit. This is also where Lua exposure #3 should be appended.
- **`FS.cpp` (+178, a real refactor):** larger filesystem-binding rework. **2026-06-20: was BLOCKING THE BUILD** — line 1's `/*` license-comment opener had been corrupted to `/*/*/*/*/*/*/ */*` (C2059). Fixed back to `/*`; now compiles. It's still a behavioral refactor I can't runtime-verify. 🟡 Exercise the FS Lua API before committing.
- Gizmo "Show labels" toggle added (default on) in the gizmo Settings popup — declutters the D3 metrics overlay.

**Recommendation:** commit the parallel work as its own commit (`feat(lua): draw helpers + api_fast additions + FS refactor`) separate from the gizmo/inspector commits, after you smoke-test the FS API. Nothing here blocks the C++ side — they coexist in the build today.

---

## Suggested commit grouping (when you're ready)
1. `fix(d3d12): sanitize stale multiviewport imgui.ini` — Framework.cpp *(user-verified)*
2. `feat(uobjecthook): property filter/group + struct copy-paste + texture-preview stub` — UObjectHook.cpp/.hpp *(2 of 3 user-verified)*
3. `feat(vr): gizmo overlay remap + tuning + auto-gizmo-on-adjust` — OverlayComponent + UObjectHook gizmo *(VR-unverified — hold until headset pass)*
4. `fix(vr): Mortal Shell 2 runtime null-guard` — FFakeStereoRenderingHook.cpp *(partial)*
5. `feat(lua): parallel-instance draw/api_fast/FS work` — bindings/* *(smoke-test FS first)*
