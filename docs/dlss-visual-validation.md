# DLSS game validation

FSR 3/4 are deferred. Custom loading-screen migration was discarded by the
user. This checklist applies to actual Bloodborne scene evaluations, not NGX
initialization or the synthetic uniform-plane backend test.

## Preconditions

- Use the exact Actions-built commit and record GPU, driver and the loaded
  model file version from the runtime log. Verify the artifact digest.
- Use `tools/windows_dlss_scene_probe.py` with isolated copies of saves and
  configuration. The source installation and save hashes must remain intact.
- Require sustained successful scene evaluations before Scaleform, with no
  subsequent temporal fallback. A run without actual evaluations fails.
- Review Native and DLSS separately. Captures of a failed DLSS run that fell
  back to Native cannot be used to approve DLSS image quality.
- Start with a safe stationary save and camera rotation. Do not add movement
  towards ledges to the basic camera test. Record deaths/loads as separate
  reset tests, rather than confusing those transitions with motion artifacts.

## Visual scenarios

| Scenario | Inspect | Required evidence |
| --- | --- | --- |
| Static camera, consecutive frames | Shimmer/flicker on fences, trees, hair, alpha-tested edges | Native and DLSS crops at the same output size; verify history converges |
| Slow and fast camera yaw/pitch | Trailing and disocclusion on silhouettes, stairs and high contrast edges | Consecutive frames during motion and after stopping |
| Character movement and attacks | Weapon trails, face/hair stability, cloth, skinning | Actual object motion coverage, moving/stationary background comparisons |
| Moving enemies/particles | Occlusion boundaries, blood/fire, transparent effects | Scene capture during motion; explicit report of unsupported vectors |
| HUD, text and menus | Text sharpness, duplicate icons, flicker/fading | Confirm these are composed after the temporal resolve |
| Original loading screens | Item icon/text stability and alpha transitions | Verify temporal scene evaluation is absent when there is no 3D scene |
| Load/teleport/area or camera cut | Old image after the transition, history contamination | Logged reset followed by a clean new history |
| Resolution/output/fullscreen/provider/preset changes | Stale depth, ghosting, wrong resource size | Logged reconfiguration/reset and continued successful evaluations |

Camera-only motion cannot certify weapons, cloth, skinning or moving enemies.
Visual review must record such limitations instead of treating finite motion
values as proof of complete object motion.

## Presets, sizes and timing

Test DLAA, Quality, Balanced, Performance and Ultra Performance when the
provider reports support. Verify the actual input and output sizes against
NGX's optimal-size query; native input must not be labelled Quality.

Cover 1920x1080, 2560x1440, 3840x2160 and a supported ultrawide resolution.
Test 30, 60 and higher frame caps, the available presentation/VSync modes,
and history resets between those configurations. Check median and tail frame
times, CPU/GPU overlap and scene streaming with captures disabled. GPU
readbacks and diagnostic logging alter timing and cannot establish performance.

## Acceptance and reporting

Record each scenario as passed, failed or untested, with commit/configuration
and evidence. A passing synthetic backend test establishes ABI/resources and
reset handling only. It does not establish game ghosting, HUD separation,
object motion or frame pacing. Game captures remain local and are not committed
or included in the public distribution.
