# UEVR `luavrlib` — Testable Features (as of 2026-06-07)

Build to test: `cmake --build build --config RelWithDebInfo --target uevr` → inject `build/bin/uevr/UEVRBackend.dll`.
Open the overlay with **Insert**. Most features live under **UObjectHook**.

> NOTE: Lua-layer features (C-API, datatypes, api_fast) were merged from the parallel review sessions in commit `8157b65` — build-verified green at review tick 5; re-verify after the merge build completes.

---

## A. UObjectHook function caller

### A1. Universal object picker (T63)  — commit `04bff16`/`0cfa998`
Any UObject-typed function parameter, and each Live Function Caller slot target, uses one picker widget.
- [ ] Inspect any object → call a function with a UObject parameter. The slot shows: a drop button, a text box, a `pick...` button, `classes` + `type filter` checkboxes, and quick-pick buttons **World / PC / Pawn / Camera**.
- [ ] Click **Pawn** (etc.) → slot fills with that object; full name wraps on the line below.
- [ ] Type a short name or `0xADDR` + Enter → resolves into the slot.
- [ ] `pick...` → searchable popup of live objects; toggle `classes` → it lists UClasses instead; toggle `type filter` off → shows all objects (ignores the param's expected class).
- [ ] Right-click the filled slot → **Copy full name**, **Send to Function Caller (as target)**.
- [ ] Drag the filled slot's object into another slot (drag source).

### A2. Generic struct caller params (T64) — commit `04bff16`/`0cfa998`/`21cee31`
- [ ] Call a function taking an **FTransform** → three grouped editable rows: `Rotation` (xyzw), `Translation` (xyz), `Scale3D` (xyz). Edit + Call writes them correctly.
- [ ] Call a function taking **FVector / FRotator / FVector2D / FColor / FIntPoint** → grouped editable row(s); width auto-correct on both UE4 (float) and UE5 (double) — no garbage.
- [ ] Function returning a struct → result prints members inline, e.g. `[Transform] {Rotation=..., Translation=..., Scale3D=...}` (commit `eb63027`).
- [ ] Function with a `WorldContext` param → auto-fills, labeled `[auto: World]`; clearing/overriding drops the auto label (commit `eb63027`).
- [ ] Non-numeric struct param (or any awkward signature) → **"Call via Lua (advanced / fallback)"** expander with an editable snippet; **Run Lua** executes it.
- [ ] Long array/string results wrap instead of clipping (commit `eb63027`).

### A3. Group functions by originating class — commit `a7170e0`
- [ ] In any object/class function view, tick **"Group by class"** (next to the filter).
- [ ] Functions split into one TreeNode per declaring class up the chain (e.g. `PlayerBP_C (N)`, `Character (N)`, `Pawn (N)`, `Actor (N)`), most-derived first, leaf class default-open, with per-class counts.
- [ ] The name filter still narrows within each group; empty groups hide.

---

## B. UObjectHook component gizmo — commits `3a41224`/`1d8c0bc`/`eb9f4b9`
- [ ] Select a SceneComponent → a screen-space translate gizmo draws at its location.
- [ ] Screen-drag the gizmo → component translates.
- [ ] Adjust the user-tunable **gizmo axis length** control.

---

## C. UObjectHook UX — commit `2ffaf62`
- [ ] **Middle-mouse drag-to-pan** inside scroll regions (object tree / lists) — hold MMB and drag to scroll.

---

## D. Reliability / crash guards
- [ ] **Lua reset crash guard** (commit `825a4a4`): overlay → Lua tab → **Reset scripts** with a broken autorun script present → no process crash; log shows `[LuaLoader] run_script threw for <path>: ... — skipping`. Log at `%APPDATA%\UnrealVRMod\<Game>-Win64-Shipping\log.txt`.

---

## E. Lua / C-API (merged `8157b65`)
- [ ] C plugins: `get_or_add_component`, `attach_to`, `get_angular_velocity` resolve (were null → crash).
- [ ] Lua: `Quaternionf.new(1,0,0,0)` is identity (the `(0,0,0,1)` 180°-Z bug fixed in `Transform.cpp`).
- [ ] ImGui Lua bindings: `ColorPicker4` / `ColorEdit4` and drawlist color calls work (no arg-type errors).
- [ ] api_fast extended lookups (`SDKFast.cpp`) behave.

---

## Known-good / known-bad renderers
- **D3D11 / UE4** (Trepang2, Ghostrunner-class): primary verification path — stable.
- **D3D12 / UE5** (Chanbara): **crashes** — parked D3D12 crash family, NOT from this day's UObjectHook/Lua work (no render/D3D12 code touched). Track separately.
- Greylock: avoid — camera oddities break universal scripts (UObjectHook UI still works, but not ideal).

## Not yet testable (needs hardware)
- Phase 4 VR (#57): real-mouse override, thumbstick pointer, OpenXR curvature — needs a headset.
