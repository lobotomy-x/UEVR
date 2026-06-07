# UEVR `luavrlib` — Engineering Progress Report

**Date:** 2026-06-06
**Branch:** `luavrlib` (UEVR — universal Unreal Engine VR injector)
**Scope:** Reflection/inspector tooling, crash-reliability engineering, and live-debug methodology

---

## Summary

Eight focused commits (+480 / −144 lines across 3 core files) delivered in one session, split between **developer-tooling features** for UEVR's live UObject inspector and **reliability engineering** that root-caused and fixed a family of in-process crashes — including a D3D12 present-hook stack overflow diagnosed directly from a crash minidump.

All work was scoped to a single subsystem (`UObjectHook` UI + the D3D12 present hook) and committed in small, isolated, individually-revertible units, while explicitly avoiding files owned by a parallel developer working the Lua/SDK layer in the same tree.

---

## 1. Inspector & tooling features

UEVR's `UObjectHook` is a live, in-game reflection browser (inspect/edit any `UObject`, call `UFunction`s, hook execution). Four enhancements:

| Commit | Feature |
|--------|---------|
| `dfe6392` | **Compact math-struct display.** `FTransform`/`FVector`/`FRotator`/`FQuat`/`FLinearColor`/`FColor` were buried several tree levels deep behind generic Inheritance/Functions/Properties nodes; now rendered as a single editable `DragScalarN` row (plus a flattened `[Transform]` composite). Component width (float on UE4, double on UE5) is **auto-derived from the struct's reported size** — no per-engine branching. |
| `b83157e` | **Function right-click context menu.** Block-execution, monitor-calls, the function-flags editor, copy name/address, and a "Send to Function Caller" action moved from inline checkboxes into a per-function context menu. Works on desktop (right-click), in VR (controller B-button maps to right-click), and via gamepad/keyboard nav. Rows show compact `[Called]/[Blocked]/[Mon N]` status badges. |
| `3457bbe` | **Unified "Function Hooks" window.** Merged the live function caller, the active block/monitor list, and the ProcessEvent monitor into one tabbed dockable window. Added a ProcessEvent "Flagged only" mode that records just the functions flagged via Monitor calls — turning a global per-call intercept into a focused per-function monitor. The two hook mechanisms (cheap per-function inline counter vs. the global ProcessEvent vtable hook) were deliberately kept independent; only the window and the flagged set were unified. |
| `3e445d0` | **"By Package" class browser.** A third sub-tab grouping every class under collapsible package headers (`/Script/Engine`, `/Game/...`) with per-package counts, alongside the existing Native/Blueprint split. Shared the per-row render path across all three views. |

---

## 2. Reliability engineering

| Commit | Fix |
|--------|-----|
| `82c210b` | **Struct-size validation.** The compact math-struct renderer matched by type *name* only, so an unrelated game struct named `Vector`/`Color` would be edited at the wrong field offsets. Now gated on the struct's reported size matching the expected scalar layout before the inline editor is offered; otherwise it falls through to the generic view. |
| `b64d713` | **Property-view exception safety.** Inspecting a component with `OverrideMaterials` (a `TArray<UMaterialInterface*>` that routinely holds **null slots**) dereferenced `obj->get_class()` with no null guard → access violation → the exception unwound past an ImGui `TreeNode`'s `TreePop`, producing "Missing TreePop()" tree corruption and an opaque "threw (unknown)". Fixed by null-guarding object-array elements and wrapping **every** recursive `TreeNode` in the property/array view with RAII `TreePop` + `try/catch`, so any throw degrades to an inline error instead of corrupting the frame. Also corrected `NameProperty` arrays from a `TArray<FName*>` (garbage-pointer deref) to by-value `TArray<FName>`. |
| `3b393b6` | **Components-list hardening.** Same crash class on the actor component tree: null/stale entries dropped before sorting, comparator + label guarded, recursion wrapped in RAII + `try/catch`. |
| `7eafd36` | **D3D12 present re-entrancy (marquee fix).** See below. |

### Marquee: D3D12 present-hook stack overflow

**Symptom:** Repeated hard crashes in D3D12 games, reported only as "new crash".

**Diagnosis (no symbols from the user required):** Recovered the game's UE crash minidump from `%LOCALAPPDATA%\<Game>\Saved\Crashes\` and symbolized it with `cdb` against the project's PDBs. The stack showed `EXCEPTION_STACK_OVERFLOW` — an infinite mutual recursion:

```
D3D12Hook::present → present_internal → present_fn
   → gameoverlayrenderer64!OverlayHookD3D3   (Steam's in-game overlay present hook)
      → D3D12Hook::present → … (∞)
```

Steam's overlay and UEVR both hook `Present` and call into each other. An existing depth guard was meant to break this, but mapping `present_internal+0x9c` back to a source line (`cdb`'s line tables) proved the recursion ran through the **early pass-through `return present_fn(...)` paths that execute before the guard** — so the guard never fired.

**Fix:** A thread-local re-entrancy guard placed ahead of the early returns; on re-entry it returns `S_OK` and skips the nested call (the outer present completes the frame). The change is **additive and depth-0 frames are untouched**, so the only altered behavior is the path that previously crashed — verified by running an unrelated D3D12 title for ~2 minutes with the fix and confirming healthy present (no regression). The D3D11 hook was checked and found already protected (its guard sits before its present call), so it was left untouched rather than churned.

---

## 3. Live-debug methodology

A repeatable, largely-autonomous loop using an MCP bridge into the injected runtime plus standard Windows debugging tools:

- **Launch + inject on demand** — drive a target game and inject the current build via the MCP `setup_game` tool, overriding the backend DLL path to bypass stale copies.
- **Crash recovery without the user** — UE packaged games write symbolized crash artifacts to `%LOCALAPPDATA%\<Project>\Saved\Crashes\UECC-*\`; located the freshest, read the `ErrorMessage`, and symbolized the `UEMinidump.dmp` with `cdb -z … -y <pdb path> -c ".ecxr; kn; .lines; ln <sym>+<off>"` to get exact recursion frames and source lines. (VR-active access violations leave no Windows Error Reporting dump, but the UE crash logs are still written — this path works where WER doesn't.)
- **Verify before claiming** — re-injected fresh builds and watched survival/health via the runtime status endpoint and backbuffer screenshots before reporting a fix as working.
- **Catch a self-inflicted footgun** — noticed a fix had been *built before its commit*, so the DLL's baked commit hash was stale and couldn't distinguish "has fix" from "no fix"; forced a commit-hash regen and rebuilt at HEAD so deployment became verifiable by hash.

---

## 4. Engineering practices demonstrated

- **Minidump forensics & root-cause analysis** — turned a vague "new crash" into an exact recursion cycle + source line.
- **Minimal-blast-radius fixes** — guards that only alter the broken path; refused to blind-edit the all-games present hot path without a reliable repro.
- **Disciplined version control** — eight small, single-purpose, individually-revertible commits with descriptive messages; no bundling.
- **Multi-developer coordination** — a second developer was actively editing the Lua/SDK layer (`lua-api/*`, `SDKFast.cpp`) in the same working tree; every commit was scoped to avoid their files, and Lua-layer crashes were diagnosed-and-flagged rather than edited.
- **Build/deploy hygiene** — handled DLL-locked-by-running-process link failures, restored a corrupted include set, and flagged (without destructively "fixing") a corrupt git object discovered via `fsck`.

---

## Commit log

```
7eafd36  D3D12Hook: break present re-entrancy with chained overlay hooks (Steam)
3b393b6  UObjectHook: harden the actor Components list against null/stale entries
b64d713  UObjectHook: stop property-view exceptions from corrupting the ImGui tree
82c210b  UObjectHook: gate compact math-struct display on exact struct size
3e445d0  UObjectHook: add "By Package" grouping to the Class Browser
3457bbe  UObjectHook: unify caller/hooks/ProcessEvent into one window + flagged PE mode
b83157e  UObjectHook: move function hook/flag controls into a right-click context menu
dfe6392  UObjectHook: compact display for core math structs (Transform/Vector/etc)
```

**Net:** +480 / −144 across `src/mods/UObjectHook.{cpp,hpp}` and `src/hooks/D3D12Hook.cpp`.

---

## Open / handed back to owner

- **D3D12 device-removed (`DXGI_ERROR_ACCESS_DENIED`) in VR** — surfaced after the present recursion was fixed; controlled retest (overlay off, fix confirmed deployed by hash) pending to separate "same Steam overlay" from "distinct D3D12-VR instability".
- **Lua-reset crash** — in the `ScriptContext`/`LuaLoader` layer owned by the parallel developer; diagnosed-and-flagged, not edited.
- **Corrupt git loose object** — flagged; recovery deferred (destructive, needs owner sign-off).
- **VR input / OpenXR overlay curvature (Phase 4)** — designed and parked pending in-headset verification.
