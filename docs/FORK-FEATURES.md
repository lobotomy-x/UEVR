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

_Months of pre-existing work on this fork's Lua/imgui stack — the largest single divergence from
upstream. Cheat sheet: docs/lua-api-cheatsheet.md (mirrored live in-game by the CheatSheet panel)._

**imgui, modernized + Lua API roughly doubled**
- dear imgui advanced ~5 years past upstream UEVR's vintage (now 1.92.x: docking, dynamic fonts,
  multi-select, modern tables) with the ABI/threading fallout of that jump ironed out.
- The Lua `imgui` binding grew to near-parity with the C++ API: full tables, tab bars, drag & drop,
  popups/modals, tree nodes, color pickers, fonts (load/push/size), style push/pop, clip rects,
  low-level draw lists + path API, item/id state, viewport sidebars, platform windows, the UEVR host
  dockspace id, and a `draw` overlay table. Optional-argument handling fixed across the board
  (omitted args no longer throw through sol2).

**`uevr.api_fast` (SDKFast) — native fast paths**
- Direct native bindings for the hot transform paths on Actors/SceneComponents
  (get/set actor & world location/rotation, offsets, sockets, root/components lookup, batch actor
  locations, cached `find_class`) that skip `process_event` reflection dispatch — a massive speedup
  for scripts touching many transforms per frame.

**Multistate / threading**
- Multiple isolated Lua states + worker-thread execution support (ScriptState), with demo scripts
  (`workers_demo` and friends) showcasing heavy per-frame workloads moved off the game thread.

**Math & struct usertypes**
- Full usertype set with metamethods and utility methods: Vector2/3/4 (float + double), Quaternion,
  Matrix4x4, Transform — arithmetic operators, `dot/cross/normalize/length`, `inverse/transpose`,
  `transform_vector`/`transform_vector4[w]`, plus `Vector3:world_to_screen()` / `world_to_ndc()`
  projection helpers.
- Automatic UStruct bindings expanded well past upstream's FVector/FRotator→vec3: FQuat→Quaternion,
  FTransform→Transform, FMatrix→Matrix, FLinearColor→vec4 all convert transparently at the
  property/param boundary (StructObject).
- **TArray support** for reading/writing array properties and function results from Lua.
- **UEnum support** (enum lookup and named values).

**Reflection power tools**
- UE4SS-style **property/function flag editing** — e.g. flip EditConst/BlueprintReadOnly off a
  property or make a function callable, from Lua and the UI.
- Blueprint/function-library access via CDOs; `:call`, `:get_property`/`:set_property` on any object.

**Embedding / tooling**
- **C++ → inline Lua**: plugins and native code can execute Lua chunks in the live state
  (thread-safe `exec_lua` through the PluginLoader C API) — the bridge the plugins below build on.
- `lua_imgui` plugin (REPL, object inspector, VR, console, bridge tabs), `cimgui_native` plugin,
  and a full in-overlay **Lua script editor** IDE (syntax highlighting, global/autorun scripts,
  Globals/Modules state-inspector tabs); PluginLoader wiring for all of it.
- **uevr-mcp**: an MCP server exposing the live game to tooling (object inspection, memory reads,
  render diagnostics, Lua exec, hooks) over HTTP/named-pipe — used heavily for live debugging.
- Demo/library scripts: freecam, devtools sidebar, workers, camera manager, crosshair, debug draw,
  common-objects helpers, and more.

## UObjectHook enhancements
- Visual transform **gizmos** (translate/rotate/scale, world/local, multi-axis, multi-select,
  click-select with candidate cycling + world-space highlight, set-movable, recenter-to-camera).
- **Function hooks** UI: block/monitor/flag functions, caller panel, active-hooks panel, ProcessEvent
  view, result display (arrays/enums/TMap/TSet/delegates).
- **Property editor**: type filter, group-by-class/type, math-struct (Transform/Vector) compact
  display, inherited-object column sizing.
- **Class browser**: class hierarchy + ScriptStruct/Enum browsing; **SDK dumpers** — pretty-printed
  JSON reflection dumps and UE4SS-style Lua LSP definitions (`---@class`/`---@field`/`---@enum` for
  editor autocompletion), per-package autosplit, decoded PropertyFlags/FunctionFlags.

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
