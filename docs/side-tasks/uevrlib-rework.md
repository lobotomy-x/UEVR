# Side-task #1 — Strip + rework `jbusfield/uevrlib`

**Repo on disk:** `I:/code/lobotomy-x/uevrlib/`
**Upstream:** https://github.com/jbusfield/uevrlib

## TL;DR

`uevrlib` is a ~28K-line pure-Lua helper library plus a small C++
`tarray_helper.dll`. Roughly half of it duplicates what the luavrlib branch
now exposes natively. The other half is real VR feature code (attachments,
IK, gunstock, reticule, controller remap, etc.) worth keeping. The work is
to:

1. Identify and **remove** the obsolete reimplementations.
2. Rewrite the keepers to use our new API surface (`uevr.api_fast`,
   GLM Vector/Quat/Matrix/Transform, TArray<T>, multistate workers, the
   Live Function Caller / drag-drop UI surface, etc.).
3. Integrate the result into the user's `Scripts/` folder alongside
   `APIUE.lua` / `API_Main.lua` / `API_Mod_*.lua`.

The user's local Scripts folder is at:
```
C:/Users/lbatv/AppData/Roaming/UnrealVRMod/UEVR/Scripts/
```
(see `docs/lua-scripts-audit.md` in this repo for what's there.)

## Inventory

### `libs/` modules (sizes from `wc -l`)

| File | Lines | Purpose | Verdict (preliminary) |
|------|-------|---------|---|
| `uevr_utils.lua`      | 4085 | grab-bag utilities | AUDIT — split into pieces |
| `remap.lua`           | 2381 | controller remap | KEEP, port to new ImGui surface |
| `scope.lua`           | 1137 | weapon scope rendering | KEEP, port |
| `reticule.lua`        |  993 | aim reticule | KEEP, port |
| `uevr_dev.lua`        |  802 | dev / debugging | OVERLAPS with our Live Function Caller |
| `widget.lua`          |  562 | UI helpers | OVERLAPS with our ImGui bindings |
| `ui.lua`              |  553 | dialog/menu handling | KEEP, port |
| `uevr_debug.lua`      |  358 | logging | REPLACE with `uevr.log_info/warn/error` |
| `libs/core/math_lib.lua` |  775 | vector/quat math fallbacks | OBSOLETE — replace with GLM bindings |
| `libs/core/params.lua`   |  379 | param helpers | OVERLAPS with function caller + StructObject |
| `libs/core/tarray.lua`   |  118 | JSON-pipe TArray helper | OBSOLETE — replace with native TArray |
| `libs/core/lerp.lua`     |  104 | lerp utilities | KEEP — pure math, no engine dep |
| `libs/core/uevr_lib.lua` |  129 | core require helpers | AUDIT |
| `attachments.lua`     |   ?  | weapon attachments | KEEP |
| `controllers.lua`     |   ?  | controller input | KEEP, but check for `uevr.api_fast` win |
| `hands.lua` / `hands_animation.lua` | ? | hand visualization | KEEP |
| `ik.lua`              |   ?  | inverse kinematics | KEEP — domain code |
| `gestures.lua`        |   ?  | gesture recognition | KEEP |
| `gunstock.lua`        |   ?  | two-handed weapon control | KEEP |
| `montage.lua`         |   ?  | animation montages | KEEP |
| `pawn.lua`            |   ?  | pawn utilities | OVERLAPS with our APIUE.lua _uobject:get_* |
| `accessories.lua`     |   ?  | accessory rendering | KEEP |
| `animation.lua`       |   ?  | animation helpers | KEEP, audit |
| `body_yaw.lua`        |   ?  | body rotation snap | KEEP |
| `flicker_fixer.lua`   |   ?  | flicker fix mod | KEEP |
| `gunstock.lua`, `interaction.lua`, `laser.lua`, `linetracer.lua`, `particles.lua` | ? | various | KEEP |
| `configui.lua`        |   ?  | per-mod config UI | KEEP, port to our ImGui surface |
| `unit_test.lua`       |   31 | tiny test harness | AUDIT |

### `plugins/tarray_helper.dll`

This is jbusfield's workaround for the fact that praydog/UEVR did not
expose `TArray<T>` directly to Lua. The DLL listens on the UEVR lua_event
channel for `GetTArray:<obj>:<method>` events, performs the TArray read in
C++, and sends back the result as a JSON string. The Lua side
(`libs/core/tarray.lua`) registers callbacks and parses the JSON.

**On the luavrlib branch this is entirely obsolete.** `ScriptUtility.cpp`
now reads every `TArrayProperty<T>` for primitives, FName, FString,
UObject*, and StructProperty natively (`prop_to_object` ArrayProperty
case in `lua-api/lib/src/ScriptUtility.cpp` lines 532+). Function returns
are unwrapped the same way. Just delete the `plugins/` folder and rip out
the JSON-event plumbing in `tarray.lua`.

### `example_*.luax` files

Twenty-ish examples demonstrating each library module. Each one uses
`.luax` as the extension so they don't auto-load; user renames to `.lua`
to enable. Useful as integration test cases — port them to also showcase
the new API surface (e.g. `example_attachments.luax` should be the
canonical example of using `uevr.api_fast.get_all_components` +
StructProperty writes + the Live Function Caller).

### `docs/` markdown

Eighteen .md files documenting each module's public API. Style is
prose + code snippets. When a module is rewritten, update its doc in
the same commit.

## Map of upstream concept → our equivalent

| `uevrlib` does | We have |
|---|---|
| `core/math_lib.lua` — `vectorSize`, `vectorDistance`, `vectorDotProduct` with `.X or .x or [1]` fallback | `Vector3f:length()`, `Vector3f:dot(v)`, arithmetic operators, `glm::distance` via `(a-b):length()` |
| `core/math_lib.lua` — quaternion helpers, rotator-to-quat conversions | `Quaternionf.quaternion(euler_vec3)`, `Quaternionf:rotator()`, `*` overloads for quat-vec, slerp, etc. |
| `core/tarray.lua` + tarray_helper.dll — async JSON pipe to read TArray | Native `TArray<T>` returned as Lua table from `ScriptUtility.cpp::prop_to_object` |
| `core/params.lua` — struct param helpers | `StructObject` usertype (`lua-api/lib/src/datatypes/StructObject.cpp`) + the in-tree function caller's per-property editor |
| `uevr_debug.lua` — log to file | `uevr.log_info(msg)` / `log_warn` / `log_error` → spdlog |
| `uevr_dev.lua` — interactive debug widget | `UObjectHook → Main → Live Function Caller` + drag-drop UObject sources |
| `widget.lua` — ImGui helper wrappers | Many already present in `bindings::open_imgui()` (`bullet_text`, `drag_float[2-4]`, `slider_int`, etc.) |
| `pawn.lua` — pawn location/rotation helpers | `uevr.api_fast.get_actor_location/rotation`, `get_root_component`, `get_component_by_class`, `get_all_components`, `get_socket_location/rotation` |
| Anything that calls `pawn:K2_GetActorLocation()` in a hot loop | Replace with `uevr.api_fast.get_actor_location(pawn)` — same call, no Lua reflection dispatch (~3-5× faster per call) |
| Background "do thing every N seconds" (uses `os.clock` loop in `on_frame`) | `uevr.spawn_worker("name", "body")` + `set_shared` for results |

## Recommended work order

1. **Delete the obsolete plumbing** (one PR).
   - Remove `plugins/tarray_helper.dll`.
   - Rewrite `libs/core/tarray.lua` to be a thin wrapper that just iterates the native Lua table returned by the property read; OR delete it entirely and update callers.
   - Delete `libs/core/math_lib.lua`; rewrite callers to use GLM types.
   - Replace `libs/uevr_debug.lua` calls with `uevr.log_info` etc.
   - Audit `libs/uevr_utils.lua` (4085 lines, biggest file) — split into focused modules, deleting anything that duplicates the new API.

2. **Port the keepers** module-by-module (one PR each).
   - For each module, swap reflection-chain calls (`pawn.K2_GetActorLocation and pawn:K2_GetActorLocation() or ...`) for `uevr.api_fast.*` where applicable.
   - Migrate vector math to GLM types.
   - Use `StructObject` for struct read/write instead of manual offset arithmetic.

3. **Re-do the examples** so each example is also a regression test for the corresponding library module under the new API. Keep them as `.luax` so users opt in.

4. **Update docs/ in lockstep** with each port. The user's `Scripts/` folder
   uses `APIUE.lua` (`_uobject:get_transform()` etc.) — wherever uevrlib has
   a similar helper, decide whether to: (a) replace uevrlib's with a call
   into APIUE, or (b) keep both with a note in the doc.

5. **Drop into user's `Scripts/`**. Final integration step: the reworked
   `libs/` folder gets placed alongside the user's `APIUE.lua` /
   `API_Mod_*.lua`. Add a small `examples/uevrlib_*.lua` that demonstrates
   the new combined surface.

## Acceptance criteria

For each module port:
- No remaining calls to `kismet_math_library:Vector_Distance` or similar Kismet fallbacks; use GLM.
- No remaining `tarray_helper.dll` references.
- No remaining `:K2_GetActorLocation()` / `:K2_GetComponentLocation()` in hot loops; use `uevr.api_fast.*`.
- All log output goes through `uevr.log_info` (not `print` or per-module log files).
- Old `.luax` example loads cleanly and visibly demonstrates the module.

For the overall rework:
- `libs/` line count drops by at least 30% (the obsolete plumbing is genuinely gone, not just renamed).
- A single combined `uevrlib_integration_test.lua` exercises every kept module without errors.
- Doc folder reflects the new API.

## References within this repo

- New Lua API surface: `lua-api/lib/src/ScriptContext.cpp`, `ScriptUtility.cpp`, `LuaLoader.cpp`, `bindings/SDKFast.cpp`.
- ImGui bindings: `src/mods/bindings/ImGui.cpp` (alphabetical, 207 entries).
- Function caller / drag-drop: `src/mods/UObjectHook.cpp`.
- Test panels (use as patterns): `Scripts/Tests/new_api_panels.lua` in the user's appdata.
- Full API summary: `docs/luavrlib-changes.md` in this repo.

## Status log

_(append-only; new entries at the top with date and what landed)_

- 2026-05-25 — initial hand-off doc written; sibling repo cloned at
  `I:/code/lobotomy-x/uevrlib/`. No code changes yet.
