# Bloodborne PC integration

## Inspected revisions (2026-10-07)

Upstream and the existing DarkIzuku fork started at
`fa904a4f9cab3753f2ec7d258cd8d271b99f6166` (`master`). Development is on
`bloodborne-pc-integration-v1`. The old repository is a read-only reference.

The old default branch is `5224a6d137d8c4efaab69f2412f4bb86227b9ab8`.
Inspected branches:

- `windows-port`: `bbb7f7231cfc53fa994a4215ff0ecff0b958c379`.
- `launcher-redesign-v2`: `bf09c21e760f4907768b9aa92986693e5c4ad202`.
- `seamless-dev`: `e090ac5e25fab486dd9e113e3a6203fda24b8241`.
- `upstream-0.3-core-port`: `f62f2da39e7f92058465abd112df26e2c6256e31`.
- `custom-loading-screens-v1`: `5009fc3e3b425cd92be458fe671469048ea3236f`.
- `offline-performance-precache-v1`: `a3c8ffcca14e51a9adda5de9350a6688e594a0f7`.
- `offline-performance-precache-buildcheck`: `f774ca5da9ed8324ba0ce79ba11c678ff7c8de6a`.
- `loading-assets-probe-20261007-a`: `c7567934ed440270cd2f755b282f74dbea2e9288`.

The modern WPF artwork, theme and layout come from the launcher-redesign
line, including its later fixes in upstream-0.3-core-port. Renderer code
from that branch must not replace bbhost's renderer.

## System map established before porting

- **Windows frontend:** old `launcher-windows/BloodborneLauncher/`
  (XAML, theme, embedded hunter background, logo and icon) becomes a WPF
  frontend to bbhost. Replace `run.bat`, `BB_*`, `bbport.ini` and
  `launcher-settings.json` contracts with bbhost's existing TOML files.
- **Preparation:** old `scripts/run_windows.py` calls `prepare.py`,
  `link_libc.py`, `link_modules.py`, content and patch compilers. bbhost's
  `core/elf.cpp`, import binding and native HLE own loading and linking.
  Extract a clear SELF to an ELF locally when necessary; do not port the
  BBPROBE memory image or PS4 module linker. The old extractor does not
  decrypt encrypted segments, and can omit non-loadable metadata.
- **Configuration:** `core/config.{h,cpp}` owns paths, servers, patches and
  plugins. `host/options.cpp` owns the typed settings, in-game menus and
  `bbhost-options.toml`. Use these actual files and preserve account and
  keybinding sections. Prevent simultaneous launcher and game writes.
- **DLSS/DLAA:** old `renderer_vulkan/vk_dlss.{h,cpp}` loads the NGX driver
  core and model at runtime, supplies render-pixel motion vectors, depth,
  jitter, reset, automatic exposure and quality modes. Keep provider logic
  behind a bbhost interface; replace Instance/Scheduler/Image dependencies.
- **FSR 3.1/4:** old `vk_fsr4.{h,cpp}` wraps FSR-Vulkan/FFX resources and
  dispatches; its common frame includes color/depth/motion/output, render and
  output sizes, jitter, exposure, reset and sharpening. The fsr-vulkan patch
  and `tools/fsr4_*` contain optimisations, to measure before adapting.
- **FSR 4.1.1:** old `renderer_vulkan/fsr411/` replays captured Vulkan
  passes. `tools/fsr4cap/` builds local assets from user-provided AMD DLLs.
  No proprietary DLL, captured model or extracted asset may enter this fork
  or a public build artifact. Redistribution and supported feature checks
  must be reviewed before implementing this provider.
- **Camera motion/jitter:** old `vk_camera_motion.*`,
  `host_shaders/camera_motion.comp`, and rasterizer/shader changes derive
  camera motion from depth and scene constants. Prefer bbhost's GX snapshots
  and engine camera data, then an explicitly defined reconstruction fallback.
- **Object motion:** old `vk_object_motion.*`, `motion_history.h` and
  shader-recompiler instrumentation retain previous skinned positions and
  match draws by GPU data. New object identity/lifetimes must come from GX
  draw objects, transforms, geometry and generation data (`host/gpu.h`,
  `hle/gx_trace.cpp`, `engine/gx_resources.*`), not PM4 register history.
- **Temporal composition:** old upscaling/UI composition tests and TAA
  shaders are reference algorithms only. bbhost has Scaleform HAL tokens,
  YEBIS tokens and frame ordering in `hle/gx_trace.cpp` and `host/render.cpp`.
  A temporal resolve must occur at an established scene boundary before UI;
  present-time FSR1 currently processes the already-composited display buffer
  and is not a suitable temporal integration point.
- **History:** reset on provider/preset/size/output/window change, missing
  camera, load/area/travel and camera cut. History ownership must use the
  existing frame/resource fences and live-resolution generation, with no
  new per-frame device-idle waits.
- **Display/effects:** `host/options.cpp` and `host/settings.h` are the
  source of actual choices: borderless/windowed, VSync, 30/60/90/120/144/off,
  ultrawide resolutions, FOV and camera scales, LOD, AA, SSAO, blur, DoF,
  chromatic aberration, vignette, bloom, fog, AO strength, saturation and
  shadow distance. Do not expose old SSR/dynamic-shadow toggles as working
  bbhost options without implementations. Logic above 60 stays at 60.
- **Live resolution:** use `engine/live_resolution.*` and GX's target resize
  broadcast. It rebuilds native resources at a safe frame boundary.
- **Mods/plugins/patches:** keep bbhost's overlays (`hle/fs.cpp`), plugin
  SDK, signatures, randomizer/boss_rush/mutators and TOML manifests; do not
  import bbport's XML patch compiler or loose-mod merger.
- **Online:** keep `net/*`, NP Matching2/Signaling/WebAPI/Auth and STUN/P2P.
  Launcher choices must preserve the custom server and isolated accounts
  per existing profile; account tokens must never be copied into logs.
- **Saves:** keep bbhost's `<data>/saves/SPRJ0005` and automatic
  `<data>/save-backups` (eight snapshots, five-minute interval). Import is
  optional, offline and must refuse any destination collision.
- **Logging:** old session logging lives in the WPF launcher and
  `scripts/run_windows.py`. Launch bbhost directly without cmd, drain both
  output streams concurrently, retain timestamped sessions and existing
  crash reports; detailed diagnostics use bbhost's own switches.
- **Loading screens:** old custom-loading branch and texture detile fixes
  are reference investigation data. bbhost snapshots Scaleform uniforms
  while building HAL tokens to avoid overwritten UI data. Original icon,
  text, fades and transitions must be visually verified before custom art.
- **Packaging:** use upstream `tools/ci_windows.sh`, MinGW toolchain,
  static runtime and official plugin targets; add a self-contained WPF
  executable in a dependent Windows job. Preserve debug symbols separately.

## Gates and evidence

First validate the unmodified Windows runtime, ABI tests and Linux tests.
The baseline workflow commit changes only CI. Only then integrate game
preparation and WPF. A successful compile alone does not establish gameplay,
online, temporal quality or loading-screen correctness.

The user-supplied CUSA03173 dump reports `APP_VER=01.09`. Its clear SELF
extracts to SHA-256
`cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86`,
not upstream's ELF hash; non-loadable program header 9 is absent. This exact
complete ELF identity is accepted alongside upstream's exact identity. All
24 expected byte sites in the shipped patches agree with this image, and a
Windows launch completed eight flips with exit 0 on an RTX 5070. This is an
initial boot check, not gameplay/visual parity certification. Per-site patch
checks remain active; unknown hashes are rejected. The launcher removes
`BBHOST_ANY_EBOOT` from its child's environment.

Pending renderer work is intentionally not advertised as available by the
first launcher build. Hardware availability requires Vulkan feature queries
and a successful provider initialization, not merely a GPU name or DLL file.

### Completed baseline and initial frontend

- Baseline `6b40d1ed3ff6487659c12f155c602d380d50a9b7`: upstream runtime
  architecture at `fa904a4f9cab3753f2ec7d258cd8d271b99f6166`, with only CI
  and the installed-MinGW-header discovery correction. Windows ABI tests
  passed under Wine, Linux tests passed. Windows run:
  https://github.com/DarkIzuku/bbhost/actions/runs/37658227224.
- Native preparation/settings integration `30b06dd`: Windows build and tests
  passed in run 37660727621; Linux passed in run 37660727655. The user's
  original clear SELF was prepared into a separate data cache and booted
  without bypassing the hash gate.
- WPF reuses the original theme/background/logo/icons and obtains choices
  from `host_options_describe`; it edits native TOML in place, retains account,
  keybinding and unknown settings, and refuses a stale settings snapshot.
- A directly supervised runtime drains both output streams into timestamped
  logs (ten retained), hides the launcher on successful process creation,
  and restores it on normal exit or crash. No cmd process is launched.
- The basic setup frontend is excluded from Windows compilation. ImGui is
  retained for native in-game PC/plugin menus. Linux keeps upstream setup.
- Online starts offline, with custom server profiles. Hunter's Dream presets
  and Discord linking are removed from this frontend; native F10 no longer
  offers Discord. Creating accounts is delegated to the configured server
  web page; no web URL is assumed. The native recovery flow remains available
  only for a compatible bbhost account service.
- Windows CI publishes a self-contained WPF launcher, tests its real window
  and crash supervision against a fake child, and combines it with runtime,
  patches/plugins, assets, diagnostics and exact toolchain metadata.

The first boot does not verify original loading icons/fades, gameplay,
multiplayer, high-refresh pacing or temporal quality. Those gates remain open.

### First player package and common upscaler

Player bundle `12185d7` passed Windows runtime/Wine, WPF and package jobs in
https://github.com/DarkIzuku/bbhost/actions/runs/37663336231.
Its native/WPF integration was tested with the user's real dump: 35 native
option cards, RTX 5070/617.14 detected, title menu reached, 240 flips, exit 0,
launcher hidden during play and restored afterwards. Test config/saves/cache
were separate from the user's game and original saves.

`9d9b0e2` routes Native/FSR 1 through `UpscalerProvider`, preserving the
existing native blit and EASU kernels. It adds real RCAS on/off/strength choices
to the native options table, automatically visible in WPF and F10. The
pure temporal policy covers size/provider/preset/resource/window resets,
explicit engine invalidations, bounded jitter and motion-unit conversion.
Engine scene hooks and temporal backends remain pending.

Windows run 37664438469 and Linux run 37664438406 passed. Wine ran four
host-side tests, including the temporal policy. Linux ctest reported 24
tests, zero failures; asset-dependent skips remain expected. WPF ran 17
checks. The combined artifact is 11502695533.

Local real-runtime checks on `9d9b0e2` reached the title menu for 240 flips
and exited 0: 1080p windowed, 720p windowed, and 720p render to 2560x1440
fullscreen with FSR 1/RCAS off at 30 FPS, then RCAS on at 60 FPS. These are
startup/presentation checks, not gameplay or frame-pacing benchmarks.
The source SELF SHA-256 was rechecked afterwards and remained unchanged.

Next priority is DLSS before FSR 3.1/4; see `dlss-integration.md` for verified
NGX/runtime findings and the remaining scene/engine prerequisites.
