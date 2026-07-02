# PR: UE5.7 D3D11 VR rendering fixes (+ D3D11 native-stereo crash, overlay)

_Draft PR body for the ~7-commit render PR onto the joeyhodge UE5.7 base. Paste into the PR after
cherry-picking per docs/RELEASE-PREP.md. Reword the intro to the target maintainer._

## Summary

Makes UE5.7 **D3D11** games render and show the in-headset overlay, plus fixes a UE5.7 native-stereo
crash and restores overlay hiding. On the merged UE5.7 path, D3D11 titles were black (scene render
target never captured) and the imgui overlay never appeared in VR; D3D12 already worked. Verified
end-to-end on Sprawl Zero (UE5.7 D3D11): stereo renders and the overlay shows in the headset.

## Changes (each commit self-contained, render files only)

1. **Skip the unsafe native-stereo PostInitProperties LocalPlayer bootstrap on UE5.7** — manually
   re-invoking the resolved PostInitProperties vtable slot on the LocalPlayer access-violates on
   UE5.7; the VEH then skip-steps the wrong function until the render thread corrupts (c0000005
   storm -> D3D12 rehook loop -> freeze/crash, seen switching ValorMortis to Native Stereo).
   Fail closed on UE5.7+ (same pattern as the existing Deadzone/Avowed/Everwind guards).

2. **Restore the UE5.7 D3D11 texture-create hook install** — an early-return for
   `is_ue57_dx11_backend()` in `allocate_render_target_texture` skipped installing the texture-create
   capture midhook, so `render_target` stayed null forever on UE5.7 D3D11 (black). Removed the
   early-return; broadened the UE5.7 install branch to D3D11 (with the dead prepare-helper detector
   re-enabled); kept the pre-hook DX12-only (its 5.7 body is the DX12 UI-duplication replay).

3. **Recover the UE5.7 D3D11 scene RT from the finalize output stack refs** — the post-hook fired but
   `texture_hook_ref` is a dangling stack pointer on D3D11 and the registers are clobbered by the
   finalize call. Disassembly showed finalize writes the fresh BufferedRT/BufferedSRV refs to stack
   locals in the current frame (`lea rdx,[rbp-0x40]; lea r8,[rbp+0x50]; call finalize`). The post-hook
   now scans a small window around rbp for a valid FTexture2DRHIRef, requiring the first 6 vtable
   slots to be code-in-module (a loose in-module check picks decoys that crash StereoStuff's
   GetNativeResource walk). Scoped to `is_ue_5_7_or_newer() && is_dx11()`.

4. **D3D11 framework imgui RT format: R8G8B8A8 -> B8G8R8A8** — the framework imgui RT was created
   R8G8B8A8_UNORM but the OpenXR FRAMEWORK_UI swapchain is B8G8R8A8_UNORM_SRGB. The per-frame copy is
   a raw `CopyResource`, which silently no-ops across format families -> the overlay quad sampled a
   black swapchain (imgui absent in VR on D3D11; D3D12 already used B8G8R8A8). Matched the format.

5. **Restore `is_drawing_anything()`** — it had been hardcoded to `true`, so the VR overlay could
   never be hidden. Returns `m_draw_ui || is_always_show_cursor()` again.

6. **OpenXR framework UI curvature (cylinder layer)** — adds the OpenXR equivalent of the OpenVR
   `SetOverlayCurvature` path (a XrCompositionLayerCylinderKHR + dispatcher), mirroring the existing
   slate cylinder. **CAVEAT: not yet confirmed curved in-headset** (reporter saw it flat; under
   investigation — the cylinder extension is enabled on VDXR but the visible result is unconfirmed).
   Consider splitting this commit out until verified.

7. **Three review-found correctness fixes** — slate cylinder centralAngle floor (1 radian -> 1 deg);
   framework cylinder dispatcher gated on `is_drawing_ui()` (don't curve the cursor slate); rbp-scan
   snapshot/restore of the global FRHITexture2D vtable on a total-miss frame.

## Testing
- Sprawl Zero (UE5.7 D3D11, VDXR/OpenXR): renders stereo + overlay shows in-headset. ✓
- Diagnosed live via memory inspection + runtime disassembly (the rbp offsets are confirmed for that
  build; the scan windows + vtable validation make it robust/safe across builds — worst case it
  recovers nothing and falls back, never crashes).

## Known limitations / not in this PR
- UE5.7 **D3D12** has a title-specific **OpenXR** gap (e.g. ValorMortis: black under OpenXR but
  renders fine under OpenVR, so the scene-RT capture itself works). Suspect is the OpenXR
  projection-layer submit path / a session conflict with the game's own OpenXR plugin; needs its own
  investigation. Users can run affected titles under OpenVR meanwhile.
- The game's own slate UI is skipped on UE5.7 (FViewportInfo RT-provider probe disabled for
  stability) — separate issue.
- #6 curvature unverified in-headset (see caveat).
