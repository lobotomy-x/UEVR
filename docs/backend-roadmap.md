# UEVR `luavrlib` — Backend (C++) Roadmap

Captured 2026-06-01 from the user's directive. This instance owns **backend/C++** while a separate instance does scripts (see `lua-scripting-handoff.md`).

**Build note:** the running game holds `build/bin/uevr/UEVRBackend.dll` open → `LNK1104` on link. **The game must be closed to build.** Batch C++ changes; build once per close/relaunch cycle. Lua-only fixes are testable live via overlay → Reset scripts (no build).
Build cmd (PowerShell, NOT git-bash — bash mangles `/m`): `cmake --build "I:/code/lobotomy-x/UEVR/build" --config RelWithDebInfo --parallel`.

---

## DONE (in current uncommitted batch — needs game-closed build)
- [x] **`draw.text`/`text_ex` → foreground draw list** (`ImGui.cpp` ~2392). Fixes Hit labels only drawing on the Debug window. (compiled clean; link blocked by running game)
- [x] **UEnum MAX flood → 1** (`UESDK/src/sdk/UEnum.cpp` `get_names` ~53): `GetValidValue` clamps invalid inputs to `_MAX`; now keep only `valid_value == i`.
- [x] **D3D11/D3D12 shown in overlay title** (`Framework.cpp` ~1702).
- [x] **ImGui default-arg regression fixed** (`ImGui.cpp`): sol2 ignores C++ default args → `begin_window`/`begin_child_window` (flags) + `table_get_column_name` + `set_scroll_here_x/y` + `set_scroll_from_pos_x/y` threw on omit/nil. All optional params → `sol::object` + default-on-nil. `imgui.begin_window("Foo")` works again.
- [x] **Instances tab → per-instance full editor**: each live instance is now a TreeNode that expands into `ui_handle_object(obj)` (property edit + function calling on the REAL instance, not the CDO). `UObjectHook.cpp` Instances tab.

## UObjectHook UX (file: `src/mods/UObjectHook.cpp`)
- [ ] **Remove "Test lua" button** (~4480, inside `ui_standard_object_context_menu`, ends ~4508). Coupled with ↓.
- [ ] **Remove "Attach Camera to / head / (Relative)" buttons** (~4821, 4832, 4889, 5292, 5299). COUPLED with the FP-camera port — the **"Attach Camera to head"** block (~4832-4885) is the user's working **head-bone FP-camera prototype** (inline Lua: head/neck socket search → SpringArmComponent → CameraActor spawn → `K2_AttachToComponent` → `ClientSetViewTarget`). Don't just delete — port its logic to the C++ FP camera below, then remove the buttons.
- [ ] **Replace the above with a proper C++ "inline Lua call" test feature** — the user wants to confirm the inline-Lua-calling path (currently `PluginLoader::get()->do_lua_string(...)`) is functional. Build a small, intentional test (e.g. a panel that runs a known inline Lua snippet against the selected object and shows the result), not ad-hoc buttons.
- [ ] **Function caller: stop requiring typed function name** (~315 `InputText("function name", ...)`). Add a searchable picker/browser of the object's/class's UFunctions (we already enumerate functions elsewhere — reuse).
- [ ] **Drag-and-drop from the MAIN tree view** doesn't work. Drag sources exist (`draw_uobject_drag_source` ~118, used in Objects-by-Class ~3284/3342/3383/3440/3648) but the main tree nodes don't attach one. Attach `draw_uobject_drag_source`/`draw_uclass_drag_source` to the main-tree node rows.
- [ ] **Fix the unfinished set-location / set-rotation test functions** (search `K2_SetActorLocation`/`SetWorldLocation`/`set_location` in the inspector test area).
- [ ] **Property-edit flag should NOT be a per-object TreeNode.** Move "edit property flags" into the **object context menu** (`ui_standard_object_context_menu` ~4470). **Same for function + function-flags** (their own context-menu entries).
- [ ] **Class viewer only edits the DEFAULT object (CDO).** In the **Instances tab** (~3605-3654), clicking an instance should **focus that instance** for full property editing + function calling — not fall back to the CDO. Currently Properties tab reads the CDO (~3564). Route the selected instance through `ui_handle_object`.

## Lua → C++ ports (the user: "world_to_screen shouldn't even be in lua… reimplement in C++, faster")
- [ ] **On-screen object picking**: pick objects by screen position via hit-result / line trace from the camera through the cursor ray; identify the screen-space selection. (Pairs with UObjectHook selection.)
- [ ] **Universal first-person camera without VR.** Ideally attach to the character **head bone** if detectable. User's heuristic (already has logic): search bone names for head/camera/weapon/fp/neck/spine, pick one whose **reference pose is centered (x/y ~0) and high up (max Z)** on the mesh. Working Lua prototype to port: `UObjectHook.cpp` ~4832-4885 (spring-arm + CameraActor + `ClientSetViewTarget`).
- [ ] **VR auto-disable when no runtime present** (don't force stereo/openxr if no HMD/runtime).
- [ ] **ImGui canvas window that Lua scripts can draw onto** (move the Lua-side "Canvas" window into C++; expose a stable full-screen overlay draw surface — ties into the `draw.*` foreground change).
- [ ] **Function browser** with: block-on-call, notify-on-call, and **hook via inline Lua or load-from-file**; **filter by calling object** (we have caller-object available in the hook path). This supersedes the ad-hoc inline-lua buttons above.
- [ ] **`world_to_screen` / `world_to_ndc` / cached viewproj → C++** (hot path; Lua version pays a `process_event` per point for the frame counter). Keep Lua shims so old scripts work.

## Hard / investigation
- [~] **Overlay breaks when the swapchain changes — even with multiviewport DISABLED.** `bb_rtv` (backbuffer RTV) is created once in `init_d3d11` (~2335) and reused every present at `on_frame_d3d11`:771. ATTEMPTED FIX: re-acquire `bb_rtv` from `get_swap_chain()->GetBuffer(0)` each present (Framework.cpp ~771). **DECISIVE DIAGNOSTIC pending live test:** render-probe now logs `sc=` (swapchain pointer) on size-change. Across a windowed↔fullscreen transition: if `sc` is UNCHANGED → ResizeBuffers, the bb_rtv re-acquire should fix it; if `sc` CHANGES → swapchain RECREATION, `get_swap_chain()` returns the OLD swapchain (probe `match=true` is a false positive), re-acquiring from it stays black → real fix is in the hook's swapchain re-tracking (`D3D11Hook` must re-hook/track the new swapchain). Only "fixed" if the overlay survives a transition ON SCREEN.
- [ ] **Multithread / multistate Lua** — "not explored or fixed properly." Revisit `ScriptState`/`ScriptContext` threading.

## Confirmed NON-bugs
- "Failed to initialize Framework on DirectX 11/12" (60× each) = benign startup retry churn (DX12 always fails on a D3D11 game; DX11 retries until the swapchain is ready, then "Framework initialized").
