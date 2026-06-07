# luavrlib branch — live progress report

_Auto-refreshed by `/loop`. Last update: 2026-05-25 20:19 local._

## Initial-goal status: **CODE COMPLETE — multiviewport + FProperty + ergonomics expansion has landed; awaiting in-game verification of latest build**

| Source-side status | |
|---|---|
| Last commit | `33b7572 docs/side-tasks: add uevr-example-project hand-off (#4)` |
| Commits this session | 22 (since `bbc43a7`) |
| Last successful build | `UEVRBackend.dll` at 2026-05-25 18:59 (no source change since `c7f2cf1`; latest commit was docs-only) |
| Working tree | clean (only intentional-ignore submodule/PDB markers + untracked dirs the user owns) |

## What's queued for your next deploy

The previous DLL was deployed and tested. Since then, four more commits
landed that the user has NOT yet exercised in-game:

| # | Item | Commit | What to look for |
|---|------|--------|------------------|
| V8 | Class Browser dockable window | `c7f2cf1` | Open via "Class Browser window" checkbox at top of UObjectHook → Main. Tab bar (Classes / ScriptStructs / Enums / Functions); filter input; each row is a drag source. |
| V9 | Function Caller dockable window | `c7f2cf1` | Open via "Function Caller window" checkbox. Same slots as the inline caller, state shared between both surfaces. |
| V10 | Text-input fallback on every drop target | `c7f2cf1` | Type a short name / full `Class /Script/...` name / `0xADDR` next to any drop target, hit Enter. Mirrors the user's Scripts/ImGui.lua object_lookup pattern. |
| V11 | Black-popup regression fix | `3dd718a` | Drag a panel out of the dock — should render its content instead of black. Caused by post-Present message pumping; fixed by pumping BEFORE UpdatePlatformWindows + WM_PAINT no-op in PlatformWindow WndProc. |
| V12 | Main dockspace host + auto-dock | `3dd718a` | All `imgui.begin_window(name)` calls from Lua now SetNextWindowDockID(host, FirstUseEver). New panels attach to the host workspace covering the game window; user can still drag-detach into popups. |

Previously verified or pending-but-no-new-symptoms:

| # | Item | Commit | Status |
|---|------|--------|--------|
| V1 | Missing-TreePop in Objects-by-class | `5a775e6` → `c0d1eb8` | Verified gone |
| V2 | `stop_worker` async, no render-thread freeze | `2af5fec` | Verified |
| V3 | Multi-viewport popup input | `3a05f64` | Verified (user: "input is working") |
| V4 | Multi-viewport popup z-order | `938f139` | WS_EX_TOPMOST forced; no negative reports |
| V5 | Live Function Caller (inline) | `0454efc` → `50529a9` | Verified |
| V6 | `uevr.api_fast.*` round-trip | `6399b98` → `c5f6b45` → `0255700` | Verified |
| V7 | Weak / Lazy / Soft / Delegate / Map / Set property handlers | `cb4ecc4` + `27bdae6` | Built, awaiting user test |

## Out of scope for the main session (deferred to side-tasks)

Tracked in `docs/side-tasks/`. Each has its own self-contained
hand-off doc; the actual work is meant to happen in a fresh session
against the sibling clones.

| Side-task | Hand-off doc | Sibling repo on disk | Status |
|---|---|---|---|
| Rework `jbusfield/uevrlib` against new API surface | `docs/side-tasks/uevrlib-rework.md` | `I:/code/lobotomy-x/uevrlib/` | Cloned, audit doc written, no code changes yet |
| Fork `elliotttate/uevr-mcp`, teach it api_fast / workers / log channel | `docs/side-tasks/uevr-mcp-fork.md` | `I:/code/lobotomy-x/uevr-mcp/` | Cloned, plan written, not yet forked on GitHub |
| Lift patterns from `praydog/REFramework` MCP/C# tooling | `docs/side-tasks/reframework-cross-pollination.md` | _(not cloned)_ | Reference-only doc; clone when the work starts |
| UE 5.x in-house example project (`UEVRExample`) | `docs/side-tasks/uevr-example-project.md` | `I:/code/lobotomy-x/uevr-example-project/` | C++/config scaffolding hand-written (.uproject, target rules, `UEVRTestSurface` plugin with `AUEVRTestActor` covering every FProperty/UFUNCTION shape, paired Lua driver). NOT yet opened in UE Editor — no .uasset files, no map content. |

## Known follow-ups (small, scoped, not blocking)

- **Inline Lua hook writing UI** (user flagged for after ergonomics) —
  let the user paste a Lua callback in-tree for a UFunction and have
  the in-game caller hook it. Currently we have `uevr.hook_create_mid`
  bound for raw addresses; the missing piece is per-UFunction inline
  editing widgets in the UObjectHook caller.
- **Function-flag / property-flag editing UI** (user flagged) —
  edit_property_flags already exists; needs polish + a function-flag
  variant.
- **TMap / TSet** read paths — currently return nil with TODO. Needs
  FScriptMapHelper / FScriptSetHelper bindings.
- **`uevr.api_fast.get_actor_transform`** — would return a full
  Transformf in one process_event; blocked on UE4-vs-UE5 FTransform
  layout (FQuat alignment, FVector size).
- **MCP / uevrlib side tasks** above.

## Recent session timeline (newest first)

```
33b7572 docs/side-tasks: add uevr-example-project hand-off (#4)
c7f2cf1 UObjectHook: dockable Class Browser + Function Caller windows + text-input drop fallback
3dd718a Multi-viewport: dockspace host + WM_PAINT no-op + pump-before-update
27bdae6 UObjectHook: surface Weak / Lazy / Soft / Delegate / Map / Set FProperty types in the UI
cb4ecc4 ScriptUtility: add Weak / Lazy / Soft / Delegate / Map / Set FProperty handlers
4fbbdc3 docs/side-tasks: hand-off docs for uevrlib rework + uevr-mcp fork + REFramework cross-pollination
b841c59 Docs: refresh multi-viewport status with both candidate fixes
938f139 Multi-viewport: force WS_EX_TOPMOST on popups so they stay above the game
0255700 api_fast: add get_all_components(actor) for one-call component listing
3a05f64 Multi-viewport: pump present-thread message queue, re-enable ViewportsEnable
f294f6f Lua: add uevr.log_info / log_warn / log_error spdlog channel
c5f6b45 api_fast: set_local_transform takes glm::quat directly (not glm::vec4)
c0d1eb8 UObjectHook: harden remaining m_meta_objects[] sites; add luavrlib-changes doc
50529a9 UObjectHook live caller: open by default, Enter resolves
5a3e0ee UObjectHook caller: pretty-print returns, drag handles on UObject/UClass results
118ae42 api_fast: extend surface; UObjectHook: pinned Live Function Caller panel
2af5fec UObjectHook + LuaLoader: outer-TreePop guard, async stop_worker
b47f49f Lua: move open_sdk_fast from add_additional_bindings to state_post_init
0454efc UObjectHook: interactive function caller widget + drag/drop UObject sources
6399b98 Lua: add uevr.api_fast.{actor,world}_{location,rotation} SDK fast path
89e2fc7 ImGui multi-viewport: disable by default, document the remaining bugs
5a775e6 UObjectHook: fix missing TreePop in Objects-by-class and harden recursion
```

## What this loop does

`/loop keep checking for the initial goal completion and update the progress report`
re-runs every ~25 minutes (no event to monitor, so dynamic mode picks
a fallback heartbeat). Each pass:

1. Snapshots commit count / latest commit / working tree / DLL build
   time / task list.
2. If anything changed (new commit, new build, new uncommitted file,
   newly-completed task), bumps the "Last update" timestamp at the
   top and edits the relevant table row.
3. If nothing changed, leaves the doc alone and just re-arms the
   wakeup.

Stop the loop by closing this session, or by telling me explicitly
to stop.
