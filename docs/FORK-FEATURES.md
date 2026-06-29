# What this fork adds over praydog/UEVR

_High-level overview for the standalone fork release (release path B in RELEASE-PREP.md). Audience:
users of the fork. Not exhaustive — the major capability areas only._

## UE5.7 VR rendering
- Merged the joeyhodge `ue57performance` UE5.7 VR rework (native stereo, D3D11/D3D12 texture-create
  capture, slate UI handling).
- **D3D11 UE5.7 now renders + shows the overlay in-headset** (see docs/PR-ue57-render-fixes.md):
  restored the texture-create hook install, recover the scene RT from the finalize output stack
  refs, fixed the framework RT format so the imgui overlay isn't a silent black copy.
- Native-stereo PostInitProperties crash fixed on UE5.7; overlay can be hidden again.
- OpenXR framework UI curvature (cylinder layer) to match the OpenVR curved panel.

## Lua scripting ecosystem (luavrlib)
- Expanded Lua API + libraries; demo scripts (freecam, devtools, workers, etc.).
- `lua_imgui` plugin (REPL, object inspector, VR, console, bridge tabs).
- `cimgui_native` plugin + a Lua script editor plugin; PluginLoader wiring.
- **uevr-mcp**: an MCP server exposing the live game to tooling (object inspection, memory reads,
  render diagnostics, Lua exec, hooks) over HTTP/named-pipe — used heavily for live debugging.

## UObjectHook enhancements
- Visual transform **gizmos** (translate/rotate/scale, world/local, multi-axis, multi-select,
  click-select with candidate cycling + world-space highlight, set-movable, recenter-to-camera).
- **Function hooks** UI: block/monitor/flag functions, caller panel, active-hooks panel, ProcessEvent
  view, result display (arrays/enums/TMap/TSet/delegates).
- **Property editor**: type filter, group-by-class/type, math-struct (Transform/Vector) compact
  display, inherited-object column sizing.
- **Class browser**: class hierarchy + ScriptStruct/Enum browsing; **SDK dumpers** to JSON and Lua
  (per-package autosplit, decoded PropertyFlags/FunctionFlags), exposed in-UI and via a Lua script.

## UI / overlay
- Drag-to-scroll decoupled from window-move (VR thumbstick / flat middle-mouse).
- Framework overlay curvature (OpenVR + OpenXR), spectator-view + 2D-screen controls, font/theme
  controls, stale-imgui.ini sanitization, multiviewport scaffolding (off by default).

## Known limitations (this fork, not release-blockers)
- UE5.7 **D3D12** has a title-specific capture gap (some games' scene RT isn't captured → black).
- The game's own slate UI is skipped on UE5.7 for stability.
- imgui multiviewport (popped-out windows) is off by default — input/focus/z-order rough edges.

## Build
- CMake + MSVC (RelWithDebInfo). `cmake --build build --config RelWithDebInfo --target uevr`.
- The DLL is injected at `%APPDATA%\UnrealVRMod\UEVR\UEVRBackend.dll`.
