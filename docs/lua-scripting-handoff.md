# UEVR `luavrlib` — Lua Scripting Handoff

**Audience:** a separate Claude instance taking over the **script-side** work (the `.lua` API + the user's script library) while the main instance fixes backend (C++) errors.
**Branch:** `luavrlib`. **Date:** 2026-06-01.

This document is the complete context for the scripting tasks: the new C++-backed Lua API you should prefer, the script library's load architecture, what has already been fixed this session, and the concrete open issues left for you.

---

## 0. Hard constraints (do not violate)

- **Do not break old scripts.** The user has a large existing library (`...\UEVR\scripts\` + `Examples\`, `UI\`, `Ignore\`). Prefer additive/compat changes. When a function moved to C++, keep the old Lua *name* as a thin shim (see `find_fast`).
- **Error isolation:** a bug in one script/panel must never down the whole module. Use `pcall`, nil-guards, and bounded loops. A `local function log` or a probe that references an undefined global will throw in the *main chunk* and kill every global the module defines (this exact bug cascaded to "nothing works" — see §4).
- **Comment rule (general, applies everywhere):** never write comments that narrate a bug fix ("changed X to fix Y"). Comments may state domain facts / rationale, terse.
- **Never `git push`/commit unless explicitly asked. Never `--no-verify`.** "Merge" means *local* merge only.
- **Multiviewport is SHELVED.** Don't touch MV paths.
- **MCP `uevr_lua_*` tools operate on a SEPARATE plugin Lua state** — they do NOT reload the game's script state. Only the overlay "Reset scripts" button or a relaunch reloads game scripts.

---

## 1. Environment & test workflow

- **Global scripts** (load for ALL games): `C:\Users\lbatv\AppData\Roaming\UnrealVRMod\UEVR\scripts\`
- **Per-game scripts:** `...\UnrealVRMod\<Game>-Win64-Shipping\scripts\` (the per-game folder `require`s the global ones via `..\UEVR\scripts`).
- **Per-game log (your primary signal):** `...\UnrealVRMod\<Game>-Win64-Shipping\log.txt`. `log_info`/`log_warn`/`log_error` and script errors land here. `print` is **not** routed anywhere useful (dead stdout) — use the loggers.
- **Reload after editing a `.lua`:** overlay → Lua tab → **Reset scripts** (no relaunch needed). A C++ change needs a full game relaunch.
- **NEW `.lua` files default to ENABLED** but still need a Reset scripts to load.
- Test games seen this session: CrabChampions (D3D11, UE), DollsNest, Trepang2. Lobby scenes have many actors → heavy w2s/draw load, good for perf testing.
- **Reading errors fast:** the error histogram is the fastest triage —
  `grep -aE "error|Exception|attempt to" log.txt | sed -E 's/\[2026[^]]*\] //' | sort | uniq -c | sort -rn | head`.
  A 4-digit count on one line = per-frame throw = the thing tanking FPS.

---

## 2. NEW C++-backed Lua API — prefer these over pure-Lua reimplementations

These were added/extended on `luavrlib`. Use them; they're faster and canonical.

### `uevr.api_fast` (native fast-path table) — `src/mods/bindings/SDKFast.cpp`
Transform & component helpers that skip `process_event` where possible:
```
get_actor_location/rotation, set_actor_location/rotation,
get_world_location/rotation, set_world_location/rotation,
add_world_offset, add_world_rotation, set_local_transform,
get_socket_location/rotation,
get_root_component, get_component_by_class, get_all_components,
destroy_actor, batch_actor_locations, is_actor, is_scene_component,
find_class(name)            -- short-name -> UClass*, deduped by outer, /Script/Engine. fallback, cached
refresh_class_cache()
```
`find_fast(input)` in `APIUE.lua` / `API_Main.lua` is now just a shim over `uevr.api_fast.find_class` + `discern_type`. The old `get_unique_short_names` + `class_short_names.json` machinery is gone.

### Matrix transforms — `lua-api/lib/src/datatypes/Matrix.cpp` (`BIND_MATRIX4_LIKE`)
```
m:transform_vector(v3)   -> v3   (w=0: drops translation, NO perspective divide)
m:transform_vector4(v3)  -> v4   (w=1: applies translation, RETURNS clip w)  <-- use for world->clip
m:transform_vector4w(v4) -> v4
```
`transform_vector4` is the basis of the working w2s path (manual perspective divide).

### `draw` table — `src/mods/bindings/ImGui.cpp` (`lua["draw"]`)
`draw.text, draw.text_ex, draw.filled_rect, draw.outline_rect, draw.line, draw.outline_circle, draw.filled_circle, draw.outline_quad, draw.filled_quad`.
**IMPORTANT (changed this session):** `draw.text`/`draw.text_ex` now render on the **foreground draw list** (`GetForegroundDrawList`) so labels overlay the game regardless of the current ImGui window. The **shape** primitives (`draw.line`, `draw.filled_rect`, …) and `imgui.draw_list_path_*` still use `GetWindowDrawList()` — i.e. they only draw where a window is current. If you find shape overlays clipped to a panel, the fix is to move them to the foreground list too (same one-line change in `api::draw::*`). The `draw_list` usertype (from `imgui.get_foreground_draw_list()`) has `add_line/rect/circle/quad/triangle/ngon` but **no `add_text`** — that's why text needed the C++ change.

### Other backend Lua-facing changes
- `log_info/log_warn/log_error` are bound as **safe `std::string` overloads** (member `:log(msg)` + free `(msg)`), routed through `API::get()->log_*` with a literal `"%s"` — no more `vsnprintf(NULL)` fast-fail crash. Safe to call with any string.
- `api.ue.msg()` no longer pops a modal `MessageBoxA` (which froze the engine thread); it logs instead.
- Struct-arg marshalling (`ScriptUtility.cpp set_property`): a fall-through bug that threw `"Invalid argument type for struct property"` for already-handled values was fixed (StructObject/table/vector cases no longer reach the final `else throw`). Passing a **real reflected struct object** as a function arg marshals correctly; passing a **Lua table** `{X=,Y=,Z=}` is the path that can still drop (see `ProjectWorldToScreen` note in §5).
- `ImGui.CalcFlags` resolves enum-name strings to ImGui flag enums (use enum versions in scripts, not raw ints).
- `sdk::UEnum::get_names()` exists in C++ (UObjectHook Enums tab, via `KismetNodeHelperLibrary`) but is **NOT yet exposed to Lua**. If a script needs enum name↔value, this is a candidate to bind.

---

## 3. Script library load architecture (read before editing imports)

**Load order = alphabetical autorun** of every top-level `.lua` in the scripts dir. Because `'U'`(0x55) < `'_'`(0x5F): `APIUE.lua` runs **before** `API_Main.lua`, and both run before `DebugDraw`/`Hit`/`MotionController`/etc.

- **`APIUE.lua`** — core reflection/type layer. Defines `_uobject`/`_uclass`/`_ustruct`/etc. metatables, `find_fast`, `discern_type`, Vector/Quat metamethods, w2s. Runs first. `Hit.lua` explicitly `require("APIUE")`s it.
- **`API_Main.lua`** — the **orchestrator**. Publishes modules as globals via coroutine loaders: `co_load(name, path)` and `delay_module_load(t)` do `_G[name] = require(name)` + `wait_for_module_init`. This coroutine approach exists to break **circular deps** and wait for game init.
- **Consumers** (`DebugDraw`, `Hit`, `MotionController`, `UI/*`) pull deps with the pattern `local X = _G.X or require("X")`, and call shared *functions* (e.g. `find_fast`, `Statics`, `Kismet`) as **plain globals**.

**Consequence / the recurring bug class:** a shared function defined `local` in APIUE is invisible to consumers unless **exported to `_G`**. Several were silently localized during an earlier cull, which broke everything downstream. When you add/keep a shared helper, export it: `_G.name = name`.

**Known duplication (the user flagged this):** `find_fast` and `discern_type` are defined **identically in both `APIUE.lua` and `API_Main.lua`** (legacy). `API_Main` also has a commented `-- _G.find_fast = find_fast` (line ~129). Consolidating to one source of truth is desirable but must not break either consumer set.

---

## 4. What was fixed this session (current state — don't redo)

**`APIUE.lua`:**
- Added a `local function log(s)` (was an undefined global `log` in the w2s probe → main-chunk throw → whole-module death → "nothing works" cascade). Routes to `uevr.params.functions.log_info`.
- `_G.find_fast = find_fast` exported (line ~84) — unblocks DebugDraw/Hit/MotionController/API_Main (they call it as a global).
- `_G.Kismet = _G.Kismet or Kismet` and `_G.Statics = _G.Statics or Statics` (lines ~408-409) — the bug: bare `Kismet =`/`Statics =` assigned to the in-scope **locals** (98/117), never to `_G`, so `Statics` was nil globally → **Hit.lua:321 threw ~9720×/frame** (FPS 4). LHS must be `_G.`.
- pc/pawn main-chunk nil-index guarded (loading at a menu where `pc` is nil no longer kills the module).
- `get_attached_components` (line ~1966) nil-guards `GetNumChildrenComponents` (nil on non-SceneComponents in some games) → stops Camera-panel spam + the `Missing PopID()` imbalance.
- **w2s camera source fix (`UpdateCachedViewProj`, ~3155):** now reads `pc.PlayerCameraManager.CameraCachePrivate.POV` (fallback `.CameraCache.POV`), pcall-guarded, falling back to the old `get_camera_comp():GetCameraView()` path. Reason: the old path read *a* `CameraComponent` off the view target and couldn't tell which of several components (e.g. a Camera-panel FPV rig) is the live camera. The PCM POV is the actually-rendered view. User confirmed "camera is better."
- **`Vector3:world_to_screen` (~3194):** for behind-camera / no-clip-w points it now returns `Vector2f.new(0,0)` instead of calling `Statics:ProjectWorldToScreen` per point. That engine call is (a) broken — it returns a constant for all inputs (Lua-table struct-arg doesn't land) — and (b) a `process_event` per behind-camera point → **the rotation stutter**. Callers already treat `(0,0)` as "skip."

**C++ (`src/mods/bindings/ImGui.cpp`):** `draw.text`/`text_ex` → foreground draw list (see §2). **Built; needs relaunch.**

**`Hit.lua`:** the `while w2s.x+w2s.y==0` spin (line ~547) was capped to a bounded `for _=1,12` loop (it hung forever when w2s returns (0,0) — the "M to draw text slows the game" symptom + blocked snap/name draws below it).

---

## 5. OPEN script issues (your work)

1. **Colors alpha bug** (`Colors.lua`). User: "alpha has definitely been changed / very low alpha."
   - `srgb_to_linear` (line ~261) **drops alpha**: it returns `Vector3f.new(r,g,b)` (line ~275). Any color routed through it loses its `.w`.
   - The "Test lerp" panel throws `Colors.lua:266: no matching function call takes this number of arguments and the specified types` — a `pow((Vector3)/1.055, 2.4)` (line ~267) or `Vector3 / scalar` (265) binding mismatch (note the line-262 comment: "Vector3 can do multiplication but not division"). Compare with `linear_to_srgb` (243) which uses `pow(saturate(color), …)`.
   - `u32_to_v` (line ~174) builds the color from a packed u32; an earlier edit set it to `Vector4f.new(r/255, g/255, b/255, a/255)`. **Recommended diagnostic:** route-trip a known color and `log` its `.w` at each stage (u32_to_v → srgb_to_linear → final draw color) to find where alpha collapses. Don't guess the direction.

2. **Snap-to-target / rotate-to-actor** (`APIUE.lua:2595`): `K2_SetWorldRotation` is **nil** on the CrabChampions component (~26×/run). Needs a method-availability guard and/or an alternate rotation call (`K2_SetWorldRotation` may not exist on all component classes; try the root component, or set via the transform fast-path `uevr.api_fast.set_world_rotation`).

3. **`load_exports` (legacy?)** — `DebugDraw.lua:1624-1645` calls a global `load_exports("UE"/"M"/"VR"/"MotionController"/"ImGui")` that is **defined nowhere**. It was masked behind earlier load failures. Evidence it's stale: `DebugDraw:3-4` already use the newer `_G.X or require("X")` pattern, and the `load_exports` calls at 2185-2188 are commented out. **Decide:** delete/replace the 1624-1645 block with `_G.X or require("X")` (likely), OR — only if the user says it's live — restore the original coroutine loader (mind the DebugDraw↔MotionController circular dep; a naive `_G[n] or require(n)` will stack-overflow on the cycle). Blocks `DebugDraw → MotionController → API_Main → UI/*`, but NOT `Hit`.

4. **Remove the `w2s_probe` from `APIUE.lua`.** It's a diagnostic scaffold living in the core lib; a bug in it (the undefined `log`) is what caused the whole-module death. Diagnostic scaffolding does not belong in the always-loaded core module.

5. **Consolidate the `find_fast`/`discern_type` duplication** between `APIUE.lua` and `API_Main.lua` (see §3) — one definition, exported once.

6. **Perf: per-point `process_event` in w2s.** `UpdateCachedViewProj` frame-caches via `Kismet("System"):GetFrameCount()` — but that call is itself a `process_event` executed on **every** `world_to_ndc` call (per point). With many points it's a storm. Replace the frame source with a cheap Lua counter incremented in an `on_pre_engine_tick`/`on_frame` callback, or (preferred — see §6) move w2s to C++.

---

## 6. Strategic direction (user's explicit intent)

> "world to screen shouldn't even be in lua… my lua side api can be reimplemented in C++ and be faster so just do that if you can."

Audit the script library for what is genuinely script-specific vs. what is generic API that belongs in C++. Strong C++-migration candidates: **world_to_screen / world_to_ndc** (hot path, per-point, currently pays a `process_event` for the frame counter), the cached view-projection update, and any other per-frame per-object math. Keep the Lua *names* as shims so existing scripts keep working. The main instance is handling backend; coordinate w2s-to-C++ with them (it spans both sides — that's why it's called out here).

Genuinely-new Lua features worth keeping in Lua: the metatable/metamethod sugar, pure-Lua math helpers, and the `apiue` reflection conveniences not covered by C++.

---

## 7. Key file map

| Thing | File:line |
|---|---|
| Core reflection + w2s + find_fast | `...\UEVR\scripts\APIUE.lua` |
| Orchestrator / module loaders | `...\UEVR\scripts\API_Main.lua` (`co_load` ~347, `delay_module_load` ~334) |
| Hit-result overlay (lines + labels) | `...\UEVR\scripts\Hit.lua` (`inner_trace_draw` ~416, in `ImGui.CanvasWindow()` ~445) |
| Colors | `...\UEVR\scripts\Colors.lua` (`u32_to_v` ~174, `srgb_to_linear` ~261) |
| 3D draw / laser | `...\UEVR\scripts\DebugDraw.lua` (`load_exports` block ~1624) |
| api_fast bindings (C++) | `src/mods/bindings/SDKFast.cpp` (table ~349, `find_class` ~384, `lua["uevr"]["api_fast"]` ~392) |
| Matrix transforms (C++) | `lua-api/lib/src/datatypes/Matrix.cpp` (`BIND_MATRIX4_LIKE` ~39) |
| ImGui/draw bindings (C++) | `src/mods/bindings/ImGui.cpp` (`api::draw` ~2392, `lua["draw"]` ~3268, path API ~2296) |
| Struct-arg marshalling (C++) | `lua-api/lib/src/ScriptUtility.cpp` (`set_property`) |
