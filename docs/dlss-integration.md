# DLSS backend integration

Current priority from the user: **DLSS, then FSR 3.1, then FSR 4**. FSR 4.1.1
and custom loading screens remain later stages. The NGX Vulkan backend is
implemented and tested on actual hardware. **In-game DLSS is still not enabled**:
the engine scene/depth/motion/jitter boundary has not been connected.

## Verified sources and local runtime

- Independent NGX implementation: old fork `upstream-0.3-core-port`
  `f62f2da39e7f92058465abd112df26e2c6256e31`,
  `gpu/shadps4/video_core/renderer_vulkan/vk_dlss.cpp`. This dynamically loads
  the driver and passes a common scene color/depth/motion/output frame.
- Checked official NVIDIA DLSS SDK interface documentation at
  `374959484e79a640feaba44c93ac8cfb0a03f5b5` (310.9.1):
  https://github.com/NVIDIA/DLSS/tree/374959484e79a640feaba44c93ac8cfb0a03f5b5/include.
  The SDK files were read outside this repository, not embedded or linked.
- Local GPU: RTX 5070, NVIDIA driver 617.14, Vulkan 1.4. Its installed
  `_nvngx.dll` exports Vulkan init, feature extension requirement, capability,
  create, evaluate, release and shutdown functions. It does not export the
  C parameter setter/getter functions; the old fork's Windows parameter ABI
  adapter is therefore relevant to a MinGW implementation.
- Existing user-owned `out/nvngx_dlss.dll` has a valid NVIDIA signature;
  SHA-256 `3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983`.
  No NVIDIA DLL/model was copied into this repository or CI artifact. Its
  presence alone is not used as proof of runtime availability.

## Implemented adaptation

`src/host/dlss_ngx.{h,cpp}` implements `UpscalerProvider` with caller-owned
bbhost Vulkan images and command buffers. The independent dynamic NGX ABI,
quality modes and frame parameters from the old fork were adapted; none of
its Instance, Scheduler, PM4, camera-register or image-cache hooks was ported.

- Discover the driver's complete instance/device extension lists. Current
  driver 617.14 exports a feature-specific instance query which returns
  `NotImplemented`; use its complete legacy list for that response or
  `OutOfDate`, preserving real failures. Never hardcode two NVX extensions.
- Use a project-specific custom-engine identifier, rather than the old
  application's numeric ID. Validate feature requirements and the actual
  Windows parameter ABI using all four setter/getter types.
- Obtain optimal render dimensions, bounds and quality/DLAA support from
  the NGX callback. For this driver, 1440p Quality requests 1707x960.
- Validate scene-only color, depth, motion, output, formats, layouts, aliasing,
  exposure, frame time, pixel jitter and declared engine jitter application.
  The existing post-Scaleform presentation path rejects temporal providers.
- Create/evaluate contexts with actual Vulkan resource views. Use render-pixel
  motion without jitter, automatic or explicit exposure and explicit reset.
  Keep NGX sharpening zero; provider-independent sharpening remains separate.
- Retire feature handles and parameters using a caller-supplied queue/fence
  retirement callback. Preserve init strings/common data until deferred NGX
  shutdown, and keep the driver's code module resident through device teardown.
  The provider does not submit queues or wait for device idle.
- If a provider cannot support/record a frame, use native presentation and
  retain pending history resets for the next valid temporal frame. Diagnostics
  report NGX failure reasons; detailed retirement logs require `BBHOST_DLSS_LOG`.

The backend is built into bbhost. It is not instantiated from an unverified
engine hook and DLSS/DLAA choices are not advertised in the player UI yet.

## Hardware and CI evidence

`tools/dlss_probe.cpp` creates synthetic Vulkan scene images and exercises the
same provider code compiled into bbhost. On the user's RTX 5070/617.14 it
initialized NGX, evaluated 96 frames and read back finite RGB output within
the test's 0.08 tolerance:

| Mode | Render (provider-reported) | Output | Maximum RGB error |
| --- | --- | --- | --- |
| Quality | 1280x720 | 1920x1080 | 0.000976562 |
| Performance | 960x540 | 1920x1080 | 0.00537109 |
| DLAA | 1920x1080 | 1920x1080 | 0.000488281 |
| Quality | 1707x960 | 2560x1440 | 0.000976562 |
| Ultra Performance | 1280x720 | 3840x2160 | 0.00683594 |
| Quality ultrawide | 1707x720 | 2560x1080 | 0.000976562 |

The uniform static infinite plane has exact zero motion and jitter-invariant
color. These are backend/ABI/resource/reset checks, not evidence about
Bloodborne's ghosting, moving characters, HUD, loading screens or pacing.
Each mode evaluates 16 frames with an explicit reset on frame 8. Context
changes and all seven deferred retirements complete; after fixing retained
init-data lifetime, two normal-log runs ended with exit 0.

Local native toolchain: LLVM-mingw `20261006`, LLVM 23/UCRT, Khronos
Vulkan-Headers `v1.4.350`; links the installed Vulkan loader by its public
exports. Windows CI uses the normal upstream MinGW toolchain. It builds
`dlss_probe`, runs `dlss_contract_test` and `upscale_dispatch_test` under
Wine, and publishes a separate diagnostic artifact without any NGX model.
Linux runs the same CPU contract/dispatch tests. The dispatcher test checks
actual fallback/reset behavior and rejection of composited HUD frames.

The probe accepts a model directory and writable cache directory. It does
not load Bloodborne, connect online, alter game files or open saves. NGX can
also use the installed driver's own normal cache. No model extraction or
public redistribution is involved.

## Adaptation requirements

Keep the independent runtime-loader and quality/DLAA ideas, but use raw
bbhost Vulkan resources through `UpscalerProvider`. Do not import shadPS4's
Instance, Scheduler, image cache, PM4 state, camera register scans or hooks.

1. Query NGX's actual instance/device extension requirements before device
   creation. Validate the selected physical device and runtime capability;
   an RTX name and two hardcoded extension names are insufficient.
2. Use the documented custom-engine project identifier initialization. The
   old hardcoded application ID must not be assumed to identify this project.
3. Validate the Windows parameter ABI in an isolated native probe before
   invoking it from the game; no NVIDIA SDK source is copied into the GPL tree.
4. Return provider-reported optimal render dimensions and supported quality
   modes rather than forcing every vendor into assumed ratios.
5. Require scene-only color, depth with verified convention, unjittered
   current-to-previous render-pixel motion, correctly applied engine jitter,
   exposure, frame time, resource views/formats and reset events.
6. Replace `scheduler.Finish()` context destruction with bbhost's existing
   resource retirement after the last submission that used the context.
   Retain native queue/barrier ownership. Avoid new per-frame idle waits.
7. On missing runtime, unsupported device or failed initialization, preserve
   native presentation and report the actual reason. Expose DLSS/DLAA in the
   native options table only after the scene pipeline and real dispatch work.

## Outstanding engine work

The current FSR 1 boundary is `render_blit_display_locked`, after Scaleform
has composed UI. It is marked `CompositePresentation`; temporal providers
are explicitly rejected there. A scene resolve must be inserted before UI,
with YEBIS ordering validated rather than guessed.

Native camera data exists: `live_resolution.cpp` already verifies the camera
blend at `0x18368b0`; FOV/aspect/near/far are at `+0x50/+0x54/+0x58/+0x5c`.
That hook alone does not identify the final projection/view matrices or their
update timing. Do not inject jitter into it without verifying projection
construction and ensuring HUD plate projection remains unjittered.

GX draw tokens carry shaders, geometry, bound objects and uniform snapshots.
`gx_resources` identities track resource lifetimes, not persistent character
or mesh identities. Previous poses must be keyed by verified engine entity/
draw provenance; matching a recycled constant-buffer address is unsafe.
Skinning, cloth and disocclusion validation remain required.

The common history/jitter/motion-unit policy is tested, but engine load,
teleport, area and camera-cut hooks are not wired to it yet. No actual
Bloodborne frame has been evaluated through DLSS in this stage.
