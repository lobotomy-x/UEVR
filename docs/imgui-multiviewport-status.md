# ImGui multi-viewport — status & plan

_Last updated: 2026-05-25 (luavrlib branch)_

## TL;DR

ImGui multi-viewport (`ImGuiConfigFlags_ViewportsEnable`) is **re-enabled**
with two candidate fixes for the previously-reported regressions:

1. **Popup input dead** → `pump_secondary_viewport_messages()` in
   `Framework.cpp`. Win32 queues messages PER THREAD, and the thread that
   called `CreateWindowEx` owns that window's queue. ImGui's CreateWindow
   callback runs on the present thread (the renderer requires it — DX11
   immediate context isn't thread-safe), but only the game thread pumps
   messages. The helper walks `platform_io.Viewports[1..]` after
   `RenderPlatformWindowsDefault` and runs `PeekMessageW` +
   `TranslateMessage` + `DispatchMessageW` per popup HWND so their
   messages actually reach `ImGui_ImplWin32_WndProcHandler_PlatformWindow`.

2. **Popup z-orders behind game window** → unconditional `WS_EX_TOPMOST`
   in `ImGui_ImplWin32_GetWin32StyleFromViewportFlags`. ImGui's default is
   `WS_EX_TOPMOST` only if `ImGuiViewportFlags_TopMost` is set; for our
   use case (VR mod, desktop is a single-app debug surface) it makes more
   sense to keep popups above the game always.

If you hit regressions: clearing the `ImGuiConfigFlags_ViewportsEnable`
bit in `Framework.cpp::IMGUICONFIGFLAGS` falls back to docking-only
(popped-out panels dock back inside the host overlay instead of becoming
top-level OS windows). The `*out_ex_style |= WS_EX_TOPMOST` line at the
bottom of `ImGui_ImplWin32_GetWin32StyleFromViewportFlags` can also be
commented out independently if topmost popups become annoying.

The renderer-side and platform-side scaffolding is intact (DX11/DX12 backends
correctly handle secondary swap chains, the present-thread path calls
`UpdatePlatformWindows` + `RenderPlatformWindowsDefault`, the Win32 callbacks
create/destroy popup HWNDs). The reason it's off is a small cluster of
unresolved **input / focus / z-order** issues that make popped-out windows
unusable in-game.

## Symptoms observed (with ViewportsEnable on)

1. **Mouse is locked to the game's host window.** Moving the cursor over a
   popped-out ImGui window does not generate ImGui mouse input — the cursor
   visibly hovers the popup but clicks land in the game window beneath it.
2. **Keyboard input never reaches popped-out windows.** WM_CHAR / WM_KEYDOWN
   for a focused popup go to the game's WndProc rather than ImGui's
   per-viewport handler.
3. **Main overlay sometimes z-orders behind the game's main window** after a
   popup is dragged out, especially with DX11.

Crash and rendering issues that *were* fixed on this branch:

- DX12 secondary windows rendered black → `RSSetViewports(3 → 1)` in
  `imgui_impl_dx12.cpp`.
- DX11 first-popup `IM_ASSERT` → swap-chain template installed in
  `imgui_impl_dx11.cpp::Init`, `memcpy` size fix in `SetSwapChainDescs`.
- Stack overflow when dragging a window out (DX11) → recursion guard
  (`thread_local s_in_d3d11_frame`, `s_in_d3d12_frame`) added to
  `on_frame_d3d11`/`on_frame_d3d12` in `Framework.cpp`. Caused by:
  `on_frame_d3d11 → RenderPlatformWindowsDefault → vd->SwapChain->Present →
  D3D11Hook (hooks ALL swapchains) → on_frame_d3d11 → ...`.

## Root-cause hypothesis for the remaining input bug

The Win32 input plumbing currently has **one** WndProc subclass —
`ImGui_ImplWin32_WndProcHandler` is wired to the game's main HWND. When ImGui
multi-viewport creates a popped-out window, it gives that HWND its own ImGui
viewport but **no WndProc hook**. ImGui upstream's
`imgui_impl_win32.cpp::ImGui_ImplWin32_CreateWindow` registers the popup
window under its own WNDCLASS (`"ImGui Platform"`) whose `WndProc` calls back
into `ImGui_ImplWin32_WndProcHandler` for that viewport's events. Confirm that
our copy of `imgui_impl_win32.cpp` (under `src/uevr-imgui/`) still does this —
if the WNDCLASS registration was removed during the earlier defensive cleanup
of `SetParent(NULL)` etc., that's the bug.

Specifically check that:

- `ImGui_ImplWin32_CreateWindow` registers the `"ImGui Platform"` class with a
  `lpfnWndProc` of `ImGui_ImplWin32_WndProcHandler_PlatformWindow`.
- `ImGui_ImplWin32_WndProcHandler_PlatformWindow` actually exists and is not
  short-circuited to `DefWindowProc`.
- The HWND's WndProc is not being clobbered by a later `SetWindowLongPtr` /
  parent-handle change.

For the z-order issue, the main overlay is rendered into the game's swap
chain. When ImGui creates a popped-out window, that popup briefly becomes the
foreground HWND (Windows promotes the active window to the top of z-order on
SetFocus). On release, the OS does not automatically restore the previous
foreground window — so the main overlay HWND (which is the game's HWND) is no
longer on top. The fix is probably a `SetWindowPos(game_hwnd, HWND_TOP, ...)`
on popup destroy.

## Re-enabling for development

To experiment without a rebuild, set the flag manually from a Lua script:

```lua
local cf = imgui.ConfigFlags
-- read-only mirror right now; if a write helper is exposed later, use it here.
print(("ConfigFlags = 0x%08x"):format(cf.DockingEnable))
```

The flag can be flipped at runtime by editing `Framework.cpp::IMGUICONFIGFLAGS`
and re-running cmake. The block of `if (io.ConfigFlags &
ImGuiConfigFlags_ViewportsEnable)` guards inside `Framework.cpp`,
`imgui_impl_dx11.cpp`, `imgui_impl_dx12.cpp`, and `imgui_impl_win32.cpp` all
become live again immediately.

## Plan for a real fix

1. **Audit the platform WndProc plumbing.** Diff our
   `src/uevr-imgui/imgui_impl_win32.cpp::ImGui_ImplWin32_CreateWindow` against
   upstream Dear ImGui v1.92.5 and re-enable any defensive removals that broke
   the per-viewport WndProc registration. Run with `SPDLOG_DEBUG` on the
   create/destroy paths to confirm the new HWND has the expected WNDCLASS.
2. **Smoke-test mouse routing.** With multi-viewport on, log `GetForegroundWindow`
   each frame from the engine-tick hook. If it's the popup HWND, ImGui should
   be reading mouse pos relative to that HWND, not the game's. Trace
   `ImGui::GetIO().WantSetMousePos` and `ImGui::GetIO().MousePos`.
3. **Z-order restoration.** Subclass the popup window's WNDPROC for
   `WM_DESTROY` to call `SetWindowPos(game_hwnd, HWND_TOP, 0, 0, 0, 0,
   SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)` so the game overlay comes back to
   the top.
4. **DX11 first-frame race.** Even with the recursion guard, DX11 secondary
   swapchains are created on the engine thread but presented on the present
   thread. Confirm that `RenderPlatformWindowsDefault` is only called from the
   present-thread `on_frame_d3d11` path (not from the engine-tick path), and
   that `m_immediate_context` is fetched per-frame (it currently is — see the
   `from_present` gating in `on_frame_d3d11`).

Once those four items are green, re-OR `ImGuiConfigFlags_ViewportsEnable`
into `IMGUICONFIGFLAGS` in `Framework.cpp`, rebuild, and validate against the
checklist in §"Symptoms observed".

## Files involved

- `src/Framework.cpp` — `IMGUICONFIGFLAGS` macro, `on_frame_d3d11`,
  `on_frame_d3d12`, multi-viewport recursion guard.
- `src/uevr-imgui/imgui_impl_win32.cpp` — `ImGui_ImplWin32_CreateWindow`,
  WndProc routing for popup windows.
- `src/uevr-imgui/imgui_impl_dx11.cpp` — secondary swap-chain template,
  `SetSwapChainDescs` memcpy size.
- `src/uevr-imgui/imgui_impl_dx12.cpp` — `RSSetViewports(1, &vp)` for
  secondary windows.
