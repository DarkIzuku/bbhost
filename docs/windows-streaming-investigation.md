# Windows streaming investigation

The user has not reproduced the reported zone stutter in bbhost. Upstream's
[known issues](https://github.com/droogie/bbhost#status) explicitly identify
pipeline compilation on first visits as a possible cause of short hitches.
Treat initial-load measurements as initial-load measurements;
they do not establish traversal performance, first visits to every area,
high-refresh pacing, or temporal-upscaler quality.

## Prior fork comparison

Read-only source: DarkIzuku/bloodborne_pc, including
`offline-performance-precache-v1` at `a3c8ffcca14e51a9adda5de9350a6688e594a0f7`
and `offline-performance-precache-buildcheck` at
`f774ca5da9ed8324ba0ce79ba11c678ff7c8de6a`.

- `8da75cd91919cdd89aca5eae5ad6c2919d6ca841` persists the driver cache and
  repairs shader-profile/preload rejection so new sessions continue recording.
  It validates pointer-free records and rebuilds incompatible permutations.
  bbhost already persists a Vulkan pipeline cache and a native stage manifest,
  recompiles current translator inputs, drops failed stages, and starts fresh
  when the driver rejects a cache. It does not have that old database-close
  path. Copying the shadPS4 cache database would introduce incompatible state.
- `85bb14f2905934961f6af6a7b456287de6a1ec0c` pre-populates staging uploads,
  retains 512 MiB and delays trimming for 30 s to avoid repeated page faults.
  bbhost already has a background memory keeper: 256 MiB staging during play,
  up to 768 MiB during loads, and delayed release. Native ownership/fences and
  device-budget handling take precedence over the old Scheduler/pool.
- The image-overlap bounds fixes in `a3c8ffc` belong to the old texture cache.
  bbhost's engine/GX resource identities and copy lifetime must be checked
  directly; replacing them with the old cache is not a performance port.
- `f774ca5` serializes detile source reads to fix streamed UI hazards. It is a
  correctness fix, not evidence of a zone-stutter improvement. Keep it as a
  loading-screen regression case. Native Scaleform constant-buffer snapshots
  are preserved; they do not by themselves prove all item icons flicker-free.

Relevant native implementation: `src/host/gpu.cpp` (driver cache and memory
keeper), `src/host/render.cpp` (stage manifest, asynchronous pipeline workers
and resource lifetime), `src/hle/gx_trace.cpp` (engine resource/draw snapshots).
Default asynchronous pipelines, native stage recording, memory keeper and
Scaleform path remain enabled. Do not force all three post-processors during
play: upstream documents faster streaming with additional hitches; WPF keeps
the upstream `streaming.all_post_processors = false` default.

## Repeatable Windows check

`tools/windows_streaming_probe.py` accepts a player package, the game folder,
an optional read-only SPRJ0005 seed, and a **new** scratch output folder.
For each run it creates separate config/data/mods/save directories, copies
the same userdata seed, prepares the executable through the native one-folder
bridge, disables online/update checks, and uses `BBHOST_EXIT_FLIP` to exit.
An explicit `--autopress` script uses bbhost's existing pad test hook; it
does not operate another application or generate native OS input.

The first run starts with an empty bbhost application cache. Subsequent runs
copy only the preceding stage manifest and Vulkan pipeline cache, and each
starts from the same save seed. Driver and OS caches are **not** cleared.
Timeout/crash/missing normal-exit markers invalidate a run. The source-save
hashes are checked afterwards. The script refuses existing output folders
and output paths overlapping its runtime, game or source saves.

No screenshots, frame readbacks or GPU timestamp profiling occur in this
measurement. Native per-second frame, main-loop wait, GX and stall counters
are written to logs. `results.json` separates full boot from the segment
after the first native world-frame marker and records loading durations.
The first statistics row after that marker is excluded because it can
straddle loading. Submit rates during uncapped loading are not gameplay FPS;
the native "over 33.3" counter actually uses a 34.5 ms tolerance. This is not
a global percentile measurement or a full input-to-display latency capture.

Example (developer diagnostic; Python 3, no additional packages):

```powershell
python tools/windows_streaming_probe.py --runtime C:/Tests/Bloodborne-PC-Windows `
  --game D:/Games/Bloodborne/data --save C:/TestSeeds/SPRJ0005 `
  --out C:/Tests/new-streaming-run --runs 3 --flips 2640 `
  --autopress 'f100:cross,f180:cross,f300:cross,f420:cross,f600:cross,f900:cross,f1600:rright:1000,f1800:lup:2000'
```

That script was selected for the tested CUSA03173 installation and save.
Do not assume it navigates another region/menu/save correctly. Verify the
native world-frame marker and the scene before comparing another route.
Debug Menu initialization is a separate opt-in `--debug-plugin` check.

## Local v0.2.15 evidence

Exact player artifact: `1a5ede84305881a759a8195fe775b6636abfd778`, upstream
v0.2.15 at `5f058af3a9134896689933a060a736c301423994`. Runtime SHA-256
`3003e323ff4ddb96e3786576ce5c207738dce6c31ebd1709337eef168133e51f`.
RTX 5070, driver 617.14, 1920x1080 windowed, Native / Off, native 60 FPS cap,
offline, gameplay plugins disabled. Three sequential 2640-flip runs from
the same copied seed used the script above; camera and movement taps were
logged. All reached the native world marker and normal exit 0. Source save
hashes remained unchanged; no measured run dumped frames.

Cold application cache: first loading screen 2.21 s, first world frame at
9.6 s. Warm runs: loading 2.01/2.09 s, first world frame at 9.8/9.9 s.
The cached stage manifest reconstructed 187 stages with zero failures in
both warm runs, and native Vulkan cache reuse was logged. First-world time
did not improve consistently; the small loading difference is not evidence
of a general speedup. Driver/OS caches were already populated by preceding
checks and were not reset.

Whole-boot counters counted 5/3/2 intervals exceeding the native 34.5 ms
tolerance; worst intervals were 104.6/75.2/82.5 ms. After the first world
marker and exclusion of its mixed statistics row, the sampled segments
(31.29/30.37/31.37 s) recorded zero such intervals, with worst intervals
30.3/31.2/24.7 ms. These include the save's initial scene/transitions; they
are not a fixed multi-area traversal or a GPU present-time capture.

A separate 1800-flip Debug Menu initialization/cache check completed three
runs with exit 0 and unchanged original saves. The official plugin generated
7126 font glyphs on eight pages, installed its native overlay and applied
3945 English strings (zero refused); this verifies initialization, not the
visual appearance of every debug menu page.

No old renderer optimization was imported on the strength of this sample.
The useful prior-cache/staging policies already have native equivalents.
The reproducible probe and detailed native counters are the new additions;
first visits to other areas remain a measured-work gate.

## Launcher diagnostics

Detailed Logs now enables native `BBHOST_FRAME_STATS=1` and a 40 ms stall
threshold alongside its existing GPU/audio diagnostics. Normal logging
still has no permanent console and keeps the existing ten-session rotation.
This provides evidence for future reports, without altering rendering,
streaming concurrency, cache compatibility, or frame pacing defaults.

## Remaining performance gates

Repeat first-visit/second-visit routes across actual areas with fixed seed,
camera, output size and settings; alternate at least two repetitions per
candidate. Distinguish shader work, CPU resource processing, storage I/O,
GPU work and synchronization before changing any of them. Confirm native
loading cards/text/alpha/fades separately from measurement runs. Preserve
the online subsystem and test 30/60/high-refresh pacing independently.
DLSS/FSR temporal game dispatch remains pending and cannot be benchmarked
as a working game option at this stage.
