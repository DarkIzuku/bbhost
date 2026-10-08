# DLSS backend integration

Current priority from the user: **finish DLSS and shadNet**. FSR 3/4 are
deferred and custom loading screens were discarded. The NGX Vulkan backend is
implemented and tested on actual hardware. The experimental pre-UI hook now
performs sustained **real Bloodborne DLAA evaluations** at 1080p and 1440p.
The player UI still does not enable DLSS: lower-resolution presets, object
motion and complete visual/performance acceptance remain unfinished.

## Verified sources and local runtime

- Independent NGX implementation: old fork `upstream-0.3-core-port`
  `f62f2da39e7f92058465abd112df26e2c6256e31`,
  `gpu/shadps4/video_core/renderer_vulkan/vk_dlss.cpp`. This dynamically loads
  the driver and passes a common scene color/depth/motion/output frame.
  Rechecked `windows-port`, `launcher-redesign-v2`, `custom-loading-screens-v1`
  and both offline performance/precache branches: their DLSS and temporal
  upscaler files have the same blobs (`72f2c43393ed11df52e57b9aa50a6cb575d5c207`
  and `750582e4b31c9c7545d473bea2ecfb9f5c4c75d3`). There is no newer backend
  hidden in these branches. The useful viewport-jitter policy was adapted
  to native GX draws; the old renderer's target redirection is not imported.
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
  its file version is **310.9.1.0**. The provider logs the version of the
  actually loaded module after creating a feature, rather than assuming that
  a file found on disk is the model used by the game.
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

The backend is built into bbhost. Opt-in diagnostics initialize it on bbhost's
actual Vulkan device, independently of scene hooks. DLSS/DLAA choices are not
advertised in the player UI yet.

`options.dlss_preset` stores DLAA, Quality, Balanced, Performance or Ultra
Performance in bbhost's existing options file. The WPF launcher exposes these
preferences in Spanish. A stored preference does not enable game DLSS or
substitute native rendering for a selected Quality preset.

## Scene integration status (October 8, 2026)

The diagnostic `BBHOST_DLSS_SCENE=1` uses the native GX resource bindings,
existing Vulkan recording/retirement and a per-submission scene pass graph.
It follows deferred lighting, compute, YEBIS, copies and offscreen Scaleform.
The intended resolve is before the first scene/UI composite and before
texture prefetch takes a snapshot, not in the post-HUD presentation blit.
Native scene vertices receive pixel jitter through their Vulkan viewport,
including model/skinning shaders without a scene constant block. Vertex
matrices stay unjittered; verified pixel projection/inverse blocks are cloned
into the existing staging ring for matching depth reconstruction.
Guest camera/culling/shadow state is not modified. History uses ordered
submission frames, resource epochs and camera discontinuities.

Actions builds through `2671069` compiled successfully for Windows and Linux.
Real-game tests using isolated saves ran 1,800 presentation flips and exited
normally with unchanged source saves, but recorded **zero scene evaluations**.
The first partial loading frame triggered the temporal fallback. `7509d51`
adds an unjittered bootstrap: projection jitter is armed only after a verified
scene/UI boundary in an earlier ordered frame. Its real-game test still ran
zero evaluations, but exposed a full-size Scaleform draw sampling the pure
scene color before YEBIS. `ca1a70e` resolves that scene source before the copy,
rather than treating its new destination as an eligible temporal input.
`b41f5c4` also applies viewport jitter and counts every real evaluation.
Its Actions-built Windows runtime loaded model **310.9.1.0** on RTX 5070 / 617.14:

- 1920x1080 / 60 cap: 1,260 real evaluations across 2,000 presentation flips,
  two history resets, no fallback, normal exit and unchanged source saves.
- 2560x1440 / 30 cap: 1,374 real evaluations across 1,800 presentation flips,
  two history resets, no fallback, normal exit and unchanged source saves.
- 3840x2160: NGX evaluations succeeded, but **visual acceptance failed**:
  the scene was black except HUD and some lights. An isolated Native run at
  the same resolution reproduced it without loading NGX. This resolution
  failure was subsequently traced to native GX target reuse, not NGX.

`7c84e5e` corrects `GXSceneContext::Initialize` while live-resolution display
buffers are enabled. The original game accepts a supplied target when its
width **or** height matches. At 3840x2160 it consequently reused the enlarged
5120x2160 display target, YEBIS wrote there, and a subsequent Scaleform copy
overwrote it with an empty scene target. Requiring **both** dimensions keeps
the normal native allocation path, actual resource dimensions and explicit
forced-target fallback. All four comparison/branch sites are checked before
installation, and both width branches are written together before display
size pinning. No PM4 interception or renderer replacement is involved.

Its Actions-built runtime passed Windows/Linux compilation and Windows ABI
and provider/scene graph tests. Two isolated 3840x2160 game runs of 1,800 flips
confirmed the world and character visible again in local captures:

- Native: no NGX model loaded, normal exit, source saves unchanged.
- DLAA: **1,106 actual scene evaluations**, two history resets, model
  **310.9.1.0**, no temporal fallback, normal exit, source saves unchanged.

Further `7c84e5e` game validation passed without temporal fallback:

- Live 1920x1080 -> 2560x1440 -> 1920x1080, after the game loaded:
  2,201 actual evaluations over 3,000 flips, four resets, all four local
  captures showing the scene, normal exit and unchanged source saves.
- 3440x1440 ultrawide: 1,728 evaluations over 2,400 flips, two resets,
  scene/HUD visible in local captures, normal exit and unchanged source saves.

These runs establish recovery from the black-scene defect at the tested
resolutions, not complete temporal image-quality or performance acceptance.

### MSI / RTSS presentation compatibility

The user reported overlay flicker since the earliest tests, including Native,
before the latest DLAA corrections. RTSSHooks64.dll and its Vulkan layer were
observed loaded (RTSS 7.3.5.28314, MSI Afterburner 4.6.6.16757).
`94b54b9` supplied a presentation-queue diagnostic. Native 3840x2160 A/B runs
used the same save/configuration, 2,700 flips, no GPU readbacks and normal
exits with source saves unchanged. The user observed a stable overlay on
the renderer queue, and reproduced flicker using the separate present queue.
A subsequent 4K DLAA run on the renderer queue was also reported stable.
It completed 2,001 actual scene evaluations with two history resets and no
temporal fallback. The Actions-built automatic-selection runtime `c16d644`
was then tested without either queue override: the log confirmed
`renderer queue for presentation (RTSS compatibility)`, 1,995 actual DLAA
evaluations over 2,700 flips, two resets, model 310.9.1.0, no fallback, normal
exit and unchanged original saves.

`c16d644` detects the active Windows RTSS hook and selects bbhost's existing
synchronized renderer-queue presentation path. Runs without RTSS retain the
separate queue. No global MSI/RTSS setting or driver profile is modified.
The log reports the actual queue and automatic compatibility selection.
`BBHOST_PRESENT_QUEUE=0/1` permits controlled diagnostic overrides; the scene
probe offers mutually exclusive matching flags. This is a workaround verified
on the recorded GPU/driver/overlay combination, not a universal RTSS diagnosis.
Captures and temporal auditing alter timing; these runs do not approve frame
pacing or quantify the performance impact of the queue selection.

The first two runs included camera rotation, an attack and death/reload.
Local captures show the HUD composed after DLAA. Matching Native references
and consecutive images are retained for further review; **complete ghosting,
moving-object coverage and in-game performance are not approved**.
`401eec6` follows GX's actual dimensions after live changes, including rollback,
rather than assuming the saved resolution is the currently allocated one.

`tools/windows_dlss_scene_probe.py` rejects runs without sustained actual
evaluations or with a temporal fallback. See [game validation](dlss-visual-validation.md)
for the requested ghosting, disocclusion, effects, HUD, reset, resolution and
pacing acceptance tests. Captures and source saves stay outside the repository.

## Native device and camera validation (October 7, 2026)

`dlss_runtime.cpp` discovers requirements before instance/device creation,
checks the selected device and initializes after native command slots exist.
Enable this development check with `BBHOST_DLSS_INIT=1` or
`upscaling.dlss_diagnostics=true`; local model search accepts
`BBHOST_DLSS_MODEL_PATH` or `upscaling.dlss_model_dir`. The cache lives under
the configured data directory. It never installs a temporal provider at the
post-Scaleform presentation boundary. Features, capabilities, retained init
storage and device shutdown retire through bbhost's ordered slot fences.

A native-game exit check found a Windows driver-binding error that the earlier
synthetic run had concealed: the driver's `NVSDK_NGX_VULKAN_Shutdown1` takes
**device plus an output for the remaining initialization count**. The public
SDK wrapper takes only device. Calling the driver with the SDK prototype left
RDX unspecified and could write through null or an unrelated pointer. The
binding now supplies owned output storage and logs result/count. This was
cross-checked locally against NVIDIA's official SDK wrapper and the installed
driver; no SDK library or driver bytes were added to this repository.
`dlss_probe ... --lifecycle-only` exercises init, optimal-size query and
retirement on another host thread without creating a feature.

With the corrected ABI, the RTX 5070/617.14 passed both that lifecycle check
and all 96 synthetic DLSS/DLAA frames again: shutdown result 1, remaining
initializations 0. The Actions-built `ebc2299` runtime also ran 900 real game
flips, loaded the isolated Hunter's Dream save, rotated the camera and retired
NGX without a crash (exit 0). Source saves remained byte-identical. This run
initialized NGX and queried 1080p Quality's 1280x720 input; **it did not
evaluate any Bloodborne frame through DLSS**.

`scene_motion.cpp` decodes the primary native GX Scene constant buffer, gated
on multiple G-buffer attachments, depth and the full scene viewport. In the
verified 1.09 layout, float words 183/187/191 hold world origin and 200..215
hold the camera-relative clip transform. The orthonormal basis at 180..191,
perspective W row, finite values and invertibility must all validate.
Reprojection includes current-origin minus previous-origin, which a simple
matrix inverse would miss. The pure history helper detects missing frames,
resource replacement, large translation and rotation. Engine area/load event
hooks and persistent object/pose identity are still required.

`BBHOST_TEMPORAL_AUDIT=1` observes real native/YEBIS/Scaleform tokens and camera
continuity. The real camera rotation produced nonzero finite render-pixel
motion (for example 15.49, 1.82 at the diagnostic center probe). Early loading
frames can interleave offscreen UI with scene draws: this audit conservatively
resets then, and does not certify a safe final scene/UI resolve boundary.

`SceneMotionPass` is a Vulkan compute pass over caller-owned depth and RG16F
motion images. It removes current jitter before reprojection, preserves pixel
motion units/Y direction, rejects invalid projection pixels and leaves padded
image regions untouched. It owns no allocator, command submission or idle
wait; the experimental scene owner provides synchronization, per-submission
descriptor sets and retirement. The device enables extended storage image
formats for RG16F when supported. This pass computes **camera motion only**;
its real-game dispatch remains conditional on a verified scene boundary.

`scene_motion_probe` reads back every output pixel on the actual GPU. Static,
near/far camera translation, current-jitter removal, yaw and points behind the
previous camera all passed, including a 65x39 viewport in an 80x48 image.
Maximum observed GPU-versus-CPU error was 0.00220 render pixels. Shader SPIR-V
was generated with Khronos glslang 16.6.0 and validated for Vulkan 1.2. The
diagnostic artifact includes the probe; the player build needs no compiler.

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

The current probe extends this to 128 frames across all five quality modes,
1080p/1440p/4K/ultrawide and native BGRA/D32S8 formats. The additional native
format case passed on the same GPU with no nonfinite output and maximum RGB
error 0.00196081. This remains synthetic backend evidence.

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

Native scene matrices are now verified from GX constants, rather than guessed
from the follow-camera hook. `scene_projection_jitter` produces a tested copy;
the experimental hook binds cloned projection/inverse constants after its
unjittered bootstrap. Consistent jitter must cover scene
geometry, depth reconstruction and deferred/post effects while keeping
Scaleform, culling, shadows and HUD plate projection unjittered.

GX draw tokens carry shaders, geometry, bound objects and uniform snapshots.
`gx_resources` identities track resource lifetimes, not persistent character
or mesh identities. Previous poses must be keyed by verified engine entity/
draw provenance; matching a recycled constant-buffer address is unsafe.
Skinning, cloth and disocclusion validation remain required.

The common history/jitter/motion-unit policy is tested, but engine load,
teleport, area and camera-cut hooks are not wired to it yet. The pre-UI DLAA
path now evaluates real frames, as recorded above. Quality, Balanced and
Performance still need a native engine render/output size split; the stored
preset must not be advertised as running until that split exists.
