# Deploy checklist · UEVR · luavrlib
_Generated 2026-05-25_

## Blockers — must be resolved before deploy
- [ ] **(high)** Mouse is locked to the game's host window
      ↳ Moving the cursor over a popped-out ImGui window does not generate ImGui mouse input — the cursor visibly hovers the popup but clicks land in the game window beneath it.
- [ ] **(high)** Keyboard input never reaches popped-out windows
      ↳ WM_CHAR / WM_KEYDOWN for a focused popup go to the game's WndProc rather than ImGui's per-viewport handler.
- [ ] **(medium)** Main overlay z-orders behind game window after popup drag-out
      ↳ Especially with DX11. Windows promotes the active window to the top of z-order on SetFocus; on release, the OS does not automatically restore the previous foreground window.

## Fix-plan acceptance
- [ ] **§1 — Audit the platform WndProc plumbing**
  - [ ] ImGui_ImplWin32_CreateWindow registers the "ImGui Platform" WNDCLASS
  - [ ] lpfnWndProc is ImGui_ImplWin32_WndProcHandler_PlatformWindow
  - [ ] Handler is not short-circuited to DefWindowProc
  - [ ] HWND's WndProc isn't clobbered by a later SetWindowLongPtr
- [ ] **§2 — Smoke-test mouse routing**
- [ ] **§3 — Z-order restoration on popup destroy**
- [ ] **§4 — DX11 first-frame race**

## Verify shipped features still work
- [ ] uevr.api_fast namespace — Direct sdk::AActor / sdk::USceneComponent accessors that bypass the generic reflection chain.
- [ ] Multistate / worker threads — Background Lua states on their own threads with a cross-state shared map.
- [ ] TArray<T> read paths fixed — ArrayProperty reads now return a real Lua table for primitives, FName, FString, UObject*, Struct.
- [ ] GLM bindings — full rewrite — Vector / Quat / Matrix / Transform with float + double precision and full metamethod surface.
- [ ] ImGui binding cleanup — bindings::open_imgui deduplicated from 498 lines / 207 unique → 207 alphabetical entries.
- [ ] Custom inline / mid hooks from Lua — uevr.hook_create_mid + uevr.call_function — raw native calls with full register frame access.
- [ ] Live Function Caller — Pinned workbench panel at the top of UObjectHook → Main with 4 fixed slots.
- [ ] Drag-and-drop on UObject / UClass TreeNodes — Every UObject TreeNode now publishes a "UEVR_UObject" drag payload; UClass entries publish "UEVR_UClass".

## Build & smoke-test
- [ ] Run `build.ps1` / `build.sh` on I:/code/lobotomy-x/UEVR
- [ ] Inject into a known-good target game and confirm overlay renders
- [ ] Run `Scripts/Tests/new_api_panels.lua` and tick every panel green
- [ ] Spawn one worker thread, verify shared map round-trip
- [ ] Re-baseline screenshots if UI surface changed

## Release
- [ ] Tag the commit · `git tag -a luavrlib-<date> -m "…"`
- [ ] Generate release notes via the dashboard's 'Draft release notes' tool
- [ ] Publish GitHub release · attach build artifacts
- [ ] Update side-task hand-offs that depended on this drop