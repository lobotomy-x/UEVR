# Issue notes export — UEVR · luavrlib
_2 issues with notes · 2026-05-26_

---

## Quick index
- Mouse is locked to the game's host window
- Plan §4: DX11 first-frame race

---

# Context pack — Mouse is locked to the game's host window
_Generated 2026-05-26 · UEVR · luavrlib_

---

## Issue
**Mouse is locked to the game's host window**

| Field | Value |
|---|---|
| Group | Symptom |
| Status | open |
| Tags | high |

Moving the cursor over a popped-out ImGui window does not generate ImGui mouse input — the cursor visibly hovers the popup but clicks land in the game window beneath it.

---

## Project context
- **Project:** UEVR · luavrlib branch
- **Baseline:** praydog/UEVR · master
- **Last updated:** 2026-05-25
- **Repo:** I:/code/lobotomy-x/UEVR

## Related features (dependency graph)
- **pump_secondary_viewport_messages()** _(in-progress, Multi-viewport (re-enabled))_ — Walks platform_io.Viewports[1..] after RenderPlatformWindowsDefault, pumping per-popup HWND messages.
  Files: src/Framework.cpp

## Fix plan
### §1 — Audit the platform WndProc plumbing _(pending)_
Diff src/uevr-imgui/imgui_impl_win32.cpp::ImGui_ImplWin32_CreateWindow against upstream Dear ImGui v1.92.5 and re-enable any defensive removals that broke the per-viewport WndProc registration. Run with SPDLOG_DEBUG on the create/destroy paths to confirm the new HWND has the expected WNDCLASS.

- [ ] ImGui_ImplWin32_CreateWindow registers the "ImGui Platform" WNDCLASS
- [ ] lpfnWndProc is ImGui_ImplWin32_WndProcHandler_PlatformWindow
- [ ] Handler is not short-circuited to DefWindowProc
- [ ] HWND's WndProc isn't clobbered by a later SetWindowLongPtr

### §2 — Smoke-test mouse routing _(pending)_
With multi-viewport on, log GetForegroundWindow each frame from the engine-tick hook. If it's the popup HWND, ImGui should be reading mouse pos relative to that HWND, not the game's. Trace WantSetMousePos and MousePos.

### §3 — Z-order restoration on popup destroy _(pending)_
Subclass the popup window's WNDPROC for WM_DESTROY to call SetWindowPos(game_hwnd, HWND_TOP, ..., SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE) so the game overlay comes back to the top.

### §4 — DX11 first-frame race _(pending)_
Even with the recursion guard, DX11 secondary swapchains are created on the engine thread but presented on the present thread. Confirm RenderPlatformWindowsDefault is only called from the present-thread on_frame_d3d11 path (not engine-tick), and that m_immediate_context is fetched per-frame.

---

## Developer notes
Test note for button verification

---

## Handoff prompt
> Paste this entire document into a new Claude session. Context above is pre-loaded.

```
You are picking up work on the UEVR (luavrlib branch).

The issue is: **Mouse is locked to the game's host window**
Severity / kind: high

Developer context / question:
Test note for button verification

All relevant context is in the sections above. Please:
1. Summarise what you understand the root cause to be
2. Propose the most targeted fix, referencing the files listed
3. List any clarifying questions you have before proceeding
```

---

# Context pack — Plan §4: DX11 first-frame race
_Generated 2026-05-26 · UEVR · luavrlib_

---

## Issue
**Plan §4: DX11 first-frame race**

| Field | Value |
|---|---|
| Group | Fix plan |
| Status | open |
| Tags | next |

Even with the recursion guard, DX11 secondary swapchains are created on the engine thread but presented on the present thread. Confirm RenderPlatformWindowsDefault is only called from the present-thread on_frame_d3d11 path (not engine-tick), and that m_immediate_context is fetched per-frame.

---

## Project context
- **Project:** UEVR · luavrlib branch
- **Baseline:** praydog/UEVR · master
- **Last updated:** 2026-05-25
- **Repo:** I:/code/lobotomy-x/UEVR

## Fix plan
### §1 — Audit the platform WndProc plumbing _(pending)_
Diff src/uevr-imgui/imgui_impl_win32.cpp::ImGui_ImplWin32_CreateWindow against upstream Dear ImGui v1.92.5 and re-enable any defensive removals that broke the per-viewport WndProc registration. Run with SPDLOG_DEBUG on the create/destroy paths to confirm the new HWND has the expected WNDCLASS.

- [ ] ImGui_ImplWin32_CreateWindow registers the "ImGui Platform" WNDCLASS
- [ ] lpfnWndProc is ImGui_ImplWin32_WndProcHandler_PlatformWindow
- [ ] Handler is not short-circuited to DefWindowProc
- [ ] HWND's WndProc isn't clobbered by a later SetWindowLongPtr

### §2 — Smoke-test mouse routing _(pending)_
With multi-viewport on, log GetForegroundWindow each frame from the engine-tick hook. If it's the popup HWND, ImGui should be reading mouse pos relative to that HWND, not the game's. Trace WantSetMousePos and MousePos.

### §3 — Z-order restoration on popup destroy _(pending)_
Subclass the popup window's WNDPROC for WM_DESTROY to call SetWindowPos(game_hwnd, HWND_TOP, ..., SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE) so the game overlay comes back to the top.

### §4 — DX11 first-frame race _(pending)_
Even with the recursion guard, DX11 secondary swapchains are created on the engine thread but presented on the present thread. Confirm RenderPlatformWindowsDefault is only called from the present-thread on_frame_d3d11 path (not engine-tick), and that m_immediate_context is fetched per-frame.

---

## Developer notes
Hypothesis: WndProc registration was stripped during defensive cleanup. Check ImGui_ImplWin32_CreateWindow for the WNDCLASS registration

---

## Handoff prompt
> Paste this entire document into a new Claude session. Context above is pre-loaded.

```
You are picking up work on the UEVR (luavrlib branch).

The issue is: **Plan §4: DX11 first-frame race**
Severity / kind: next

Developer context / question:
Hypothesis: WndProc registration was stripped during defensive cleanup. Check ImGui_ImplWin32_CreateWindow for the WNDCLASS registration

All relevant context is in the sections above. Please:
1. Summarise what you understand the root cause to be
2. Propose the most targeted fix, referencing the files listed
3. List any clarifying questions you have before proceeding
```