# Release / PR prep — strategy & status

_Started 2026-06-29 (autonomous). Living doc — update as cleanup lands._

## The scope problem (read this first)

`auto-roadmap-2026-06-24` is **429 commits / 2446 files / ~440k insertions** ahead of `master`. A
single PR of that to `origin` (praydog/UEVR) is **not reviewable or mergeable** — no maintainer
takes a 440k-line drop. The diff is three stacked layers:

1. **praydog/UEVR** — `origin` (upstream base, fetch-only; push goes to the lobotomy-x fork).
2. **joeyhodge UE5.7 rework** — `joeyuevr/ue57performance`, merged in at `98f7949`. This is the bulk
   of the new VR-rendering code (UE5.7 stereo, D3D11/D3D12 texture-create capture, etc.). It is
   itself a large third-party effort; we sit on top of it.
3. **lobotomy-x work** — the Lua API/imgui ecosystem, UObjectHook enhancements, UI, plugin loader,
   uevr-mcp, and this session's UE5.7 render fixes.

## Recommended strategy (don't try to upstream everything)

- **A. UE5.7 D3D11/D3D12 render fixes → PR to whoever owns the UE5.7 base (joeyhodge first).**
  These are self-contained, high-value, and directly fix the merged UE5.7 rendering. They depend on
  the joeyhodge UE5.7 texture-create path, so they belong on `joeyuevr/ue57performance` (or praydog
  IF praydog has since merged UE5.7 D3D11). This is the one genuinely upstreamable, reviewable PR.
  Candidate commits (all on FFakeStereoRenderingHook.cpp / Framework.cpp / OverlayComponent.cpp /
  D3D11Component.cpp / D3D12Component.cpp — no dependency on the Lua/UI ecosystem):
    - `b11b4f9` skip unsafe native-stereo PostInitProperties bootstrap on UE5.7 (crash fix)
    - `b727eff` restore the UE5.7 D3D11 texture-create hook install (merge regression)
    - `9a34192` recover the UE5.7 D3D11 scene RT from finalize output stack refs (the core fix)
    - `642ffbb` D3D11 framework RT format BGRA -> imgui renders in VR (CopyResource family fix)
    - `c2da031` is_drawing_anything() restore (overlay can hide again)
    - `470c372` OpenXR framework UI curvature (cylinder layer)
    - `8dae30a` 3 review-found correctness fixes
  These ~7 commits are a coherent, ~700-line, reviewable PR. They all DIRECTLY modify joeyhodge's
  UE5.7 code (e.g. `b727eff` removes a joeyhodge early-return; `9a34192`/`642ffbb` extend joeyhodge's
  D3D11 capture), so they apply on top of the joeyhodge base by construction. Validated by inspection
  (all touch only render files; none reference the Lua/UI ecosystem). Recommended local prep (user
  runs the push):
  ```
  git fetch joeyuevr
  git checkout -b ue57-render-fixes joeyuevr/ue57performance
  git cherry-pick b11b4f9 b727eff 9a34192 642ffbb c2da031 470c372 8dae30a
  # resolve any conflicts (c2da031 is_drawing_anything + 642ffbb RT format may touch praydog-era
  # lines that differ on joeyhodge's base), build, then open the PR to joeyuevr manually.
  ```
  DO NOT push or open the PR autonomously (irreversible/outward-facing).

- **B. The lobotomy-x ecosystem (Lua, UI, UObjectHook, mcp) → standalone fork RELEASE, not upstream.**
  Too divergent and opinionated to upstream. Ship it as the lobotomy-x fork's own tagged release
  with a CHANGELOG, not a praydog PR.

- **C. Truly base-level fixes → small targeted praydog PRs** only if any exist that are independent
  of both the joeyhodge merge and the Lua ecosystem. (Audit pending — likely very few.)

## Open render bugs (NOT release-blockers, document as known issues)

- ValorMortis (UE5.7 D3D12): scene render_target never captured (acquire_scene_target_resource null
  every frame -> bootstrap) -> black. D3D12 analog of the D3D11 capture bug; needs live disasm.
- Game's own slate UI missing on UE5.7 (FViewportInfo RT-provider probe deliberately skipped for
  stability, FFakeStereoRenderingHook.cpp:14694).
- Framework curvature reported flat by user despite cylinder extension enabled — under diagnosis.
See `.remember/remember.md` for full root-cause notes.

## Cleanup TODO (autonomous-safe, in progress)
- [x] Reverted temporary diagnostics (curv-diag, d3d12-rt-diag) — tree clean at 8dae30a.
- [~] Lua API_Main.lua:207 `GetInputMouseDelta` nil spam is a RUNTIME script (not in the repo — only
      docs reference API_Main). It's a deployed luavrlib script in %APPDATA%\UnrealVRMod\...\scripts;
      fix belongs to the luavrlib source/deploy, not this repo. Out of scope for the render PR.
- [x] Audited session-touched render files — clean, no leftover [*-diag]/TEMPORARY/debug prints
      (only upstream imgui comments + legitimate UI snprintf).
- [x] Drafted the render PR body -> docs/PR-ue57-render-fixes.md (paste after cherry-pick).
- [ ] Confirm build is warnings-clean (RelWithDebInfo) before release — needs a full rebuild.
- [ ] (Optional, needs user) dry-run the cherry-pick onto joeyuevr/ue57performance to surface
      conflicts before the real PR — not done autonomously (risk of a messy git state while away).

## Do NOT do autonomously
Pushing, opening PRs, force-pushing, tagging releases, `git gc` (a backup tag exists per memory) —
all irreversible/outward-facing. Prepare locally; the user drives the publish.
