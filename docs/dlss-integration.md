# DLSS next-stage integration

Current priority from the user: **DLSS, then FSR 3.1, then FSR 4**. FSR 4.1.1
and custom loading screens remain later stages. DLSS is not currently enabled
or advertised as implemented in the player build.

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
  presence alone does not establish a successful NGX context or dispatch.

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
