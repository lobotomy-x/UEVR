# UEVR `luavrlib` — Daily Progress Report (2026-06-07)

**Branch:** `luavrlib` · **Repo:** `I:\code\lobotomy-x\UEVR`
**Scope:** UObjectHook reflection-tooling features, function-caller struct support, crash hardening, and consolidation of the parallel C-API/Lua review work.

Work this day spanned several concurrent sessions sharing one working tree (one backend/UObjectHook track, one ImGui/D3D12 track, and several Lua/C-API review tracks). This report covers the consolidated result on `luavrlib`.

---

## 1. Function caller — major feature work (UObjectHook)

| Commit | Feature |
|--------|---------|
| `04bff16` | **T63 universal object picker.** One reusable widget for every UObject-typed parameter and each Live Function Caller slot: drop target + short-name/`0xADDR` text resolve + searchable pick popup + common-object quick-picks (World/PC/Pawn/Camera) + instances↔classes toggle + type-filter toggle + send-to-caller + wrapped long names. |
| `04bff16` | **T64 generic struct caller params.** `collect_struct_leaves` recursively flattens any struct to scalar leaves (descends nested structs, so `FTransform` flattens to Rotation/Translation/Scale3D), driving each leaf's width from its own `FProperty` — no UE4/UE5 padding guesswork. Render and encode share the leaf order; every nested write is bounds-checked against the param-buffer size. Non-numeric structs fall back to an editable inline-Lua snippet run via `do_lua_string`. |
| `eb63027` | Caller polish: `WorldContext` auto-resolves to the world (labeled `[auto: World]`); struct return values print members inline; long results wrap. |
| `21cee31` | Fixed UE5 double-width `FQuat`/`FVector4` in the struct-return display (float read on 4-double data → garbage). |
| `0cfa998` | T63/T64 review fixes: `gather_common_objects` memoized per ImGui frame (was a `process_event` ×N/frame); `collect_struct_leaves` walks the SuperStruct chain so inheriting structs encode completely. |
| `a7170e0` | **Group functions by originating class** — a toggle that splits a function list into per-declaring-class TreeNodes (most-derived first, with counts) instead of one flat alphabetical list. |

**Adversarial review (T63/T64):** a 4-lens, 12-agent verification pass (ImGui ID/stack balance, encode memory-safety, UE null-safety, logic-vs-spec) found **zero** memory-safety or ImGui-balance bugs and three low-severity issues, all fixed in `0cfa998`.

---

## 2. UObjectHook UX & gizmo

| Commit | Change |
|--------|--------|
| `3a41224` / `1d8c0bc` / `eb9f4b9` | Screen-space **translate gizmo** for the selected SceneComponent: draw, screen-drag-to-translate, and a user-tunable axis length. |
| `2ffaf62` | **Middle-mouse drag-to-pan** in scroll regions. |

---

## 3. Reliability

| Commit | Fix |
|--------|-----|
| `825a4a4` | **Lua reset crash guard.** `reset_scripts` called `run_script` unguarded; a script that corrupts the `package` table makes `run_script`'s unprotected `package.path` restore panic → uncaught C++ exception → frame-loop crash. Wrapped the per-script call in try/catch so a bad script logs and skips. (Root — restore outside the try in the Lua layer's `ScriptState::run_script` — flagged for the Lua track.) |

---

## 4. Lua / C-API review consolidation — `8157b65`

Merged the stopped Lua/C-API review sessions' working tree (review ticks 1–5, build-verified green at tick 5):
- **PluginLoader:** wired 3 missing C-API function pointers (`get_or_add_component`, `attach_to`, `get_angular_velocity`) that were null → crash for C plugins.
- **datatypes/Transform:** fixed identity quaternion (`Quaternion(0,0,0,1)` is a 180° rotation under glm's `(w,x,y,z)` ctor → `(1,0,0,0)`).
- **ImGui.cpp:** fixed a cluster of broken color bindings (`ColorPicker4`/`ColorEdit4`/drawlist `ImU32`).
- **SDKFast:** `api_fast` read-only lookup extensions.
- **Dead-code removal:** unused `MathUtility.hpp` (231 lines, duplicate of `utility::math`), `scriptutility.hpp.txt`, scratch debug logs.

**Flagged for owner decision (not auto-applied):** ABI mid-struct insertions in `UEVR_PluginFunctions`/`UEVR_FPropertyFunctions` (move new members to struct end to avoid breaking pre-compiled fork plugins); `ScriptState` `os.*` sandbox left disabled + `debug` lib open (security); orphaned C-API (`create_script_state`, commented `UEVR_AActorFunctions`, `UObjectReference`).

---

## 5. Live verification status

- D3D11/UE4 (Trepang2) is the primary verification path; the game's engine tick pauses while unfocused, so MCP-driven game-thread reflection calls only service when the window is focused.
- **Unverified live:** the T64 encode/write path on a real `process_event` (games kept closing/unfocusing). Bounds-checked but warrants a focused in-overlay pass.
- **D3D12/UE5 (Chanbara) still crashes** — the parked, multi-session-old D3D12 crash family; **none of this day's UObjectHook/Lua commits touch render or D3D12 code**, so it is unrelated to today's work.

---

## 6. Commit log (this day)

```
8157b65  Lua/C-API: consolidate parallel-session review work (ticks 1-5)
eb9f4b9  feat(uobjecthook): user-tunable gizmo axis length
1d8c0bc  feat(uobjecthook): gizmo screen-drag to translate component
3a41224  feat(uobjecthook): screen-space translate gizmo (draw)
a7170e0  feat(uobjecthook): "Group by class" toggle for function views
2ffaf62  feat(uobjecthook): middle-mouse drag-to-pan for scroll regions
825a4a4  LuaLoader: don't let a Lua panic during reset_scripts crash the frame loop
0cfa998  UObjectHook: address T63/T64 review findings (perf + inheriting-struct completeness)
04bff16  UObjectHook: universal object picker (T63) + generic struct caller params (T64)
21cee31  UObjectHook: fix UE5 double-width Quat/Vector4 in struct return display
eb63027  UObjectHook caller: WorldContext auto-resolve, struct return values, wrapped results
```

All commits local to `luavrlib`; **not pushed**.

---

## 7. Open / next

- Push `luavrlib` (awaiting user go).
- Owner-decision items from the C-API review (ABI struct ordering, `os.*` sandbox, orphaned C-API).
- Parked: D3D12/UE5 crash family.
- Phase 4 VR (needs headset).
- Untracked scratch (build dirs, `ClassLibrary1/2`, example plugins, extra docs) left out of the merge — triage separately.
