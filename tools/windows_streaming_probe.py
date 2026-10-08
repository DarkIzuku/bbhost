#!/usr/bin/env python3
"""Opt-in Windows load/cache comparison. Never operates on the player's data.

Pass a player package, one game folder, and a NEW scratch output folder.
An optional SPRJ0005 seed is read only; every run receives a fresh copy.
Uses bbhost's pad test hook, frame counters and bounded flip-count exit.
Does not inject native OS input, disable validation, or change the renderer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def quote(path):
    return json.dumps(str(path).replace("\\", "/"), ensure_ascii=False)


def seed_files(seed):
    files = sorted(seed.glob("userdata*"))
    if not files or any(not re.fullmatch(r"userdata\d{4}", p.name) or
                        not p.is_file() or p.is_symlink() for p in files):
        raise ValueError("Save seed must contain ordinary userdataNNNN files.")
    return files


def statistics(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    pattern = (r"\[bbhost\] frames: ([\d.]+) s: (\d+) flips .*?"
               r"max ([\d.]+) ms, \d+ over 16.7, (\d+) over 33.3")
    def counters(segment):
        rows = re.findall(pattern, segment)
        seconds = sum(float(r[0]) for r in rows)
        return {
            "recorded_seconds": round(seconds, 2),
            "recorded_flips": sum(int(r[1]) for r in rows),
            "submit_fps": round(sum(int(r[1]) for r in rows) / seconds, 2) if seconds else None,
            "worst_flip_interval_ms": max((float(r[2]) for r in rows), default=None),
            # Native counter uses >34.5 ms tolerance, although its label is 33.3.
            "intervals_over_34_5_ms": sum(int(r[3]) for r in rows),
        }
    world = re.search(r"world: the first in-game frame, flip (\d+), ([\d.]+) s after start", text)
    after = text[world.end():] if world else ""
    # Discard the first statistics row following the world marker: that
    # interval can include the final part of the preceding loading screen.
    mixed = re.search(pattern, after)
    after = after[mixed.end():] if mixed else ""
    return {
        **counters(text),
        "whole_boot_included": True,
        "world_evidence": bool(world),
        "first_world_seconds": float(world.group(2)) if world else None,
        "load_seconds": [float(t) for t in re.findall(r"loading: ([\d.]+) s", text)],
        "after_first_world": counters(after),
        "cache_and_load_events": [line for line in text.splitlines() if any(
            key in line for key in ("stage manifest:", "pipeline cache", "memory keeper:",
                                    "autopress:", "world: the first in-game", "loading:", "exit: flip"))],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True, help="Player package directory")
    parser.add_argument("--game", type=Path, required=True, help="One Bloodborne installation folder")
    parser.add_argument("--out", type=Path, required=True, help="NEW scratch directory; never reused")
    parser.add_argument("--save", type=Path, help="Read-only SPRJ0005 seed, copied for each run")
    parser.add_argument("--autopress", default="", help="Native BBHOST_AUTOPRESS script; no default menu assumption")
    parser.add_argument("--flips", type=int, default=1800)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--runs", type=int, default=3, help="One cold application cache and subsequent warm runs")
    parser.add_argument("--debug-plugin", action="store_true", help="Opt-in official plugin initialization check")
    args = parser.parse_args()
    runtime, game, output = (p.resolve() for p in (args.runtime, args.game, args.out))
    seed = args.save.resolve() if args.save else None
    if args.runs < 2 or args.runs > 6 or args.flips < 1 or args.timeout < 1:
        raise ValueError("Use 2..6 runs, positive flips and timeout.")
    # Prevent writes in/over either source, or a parent containing a source.
    for protected in (runtime, game, seed):
        if protected and (output == protected or protected in output.parents or output in protected.parents):
            raise ValueError("Scratch output must be separate from runtime, game and save sources.")
    exe = runtime / "bbhost.exe"
    if not exe.is_file() or not game.is_dir():
        raise ValueError("Runtime executable or game folder is missing.")
    files = seed_files(seed) if seed else []
    original = {p.name: digest(p) for p in files}
    output.mkdir(parents=True, exist_ok=False)
    (output / "seed-hashes.json").write_text(json.dumps(original, indent=2), encoding="utf-8")
    metadata = runtime / "build-info.json"
    info = {"executable_sha256": digest(exe), "flips": args.flips, "autopress": args.autopress,
            "debug_plugin": args.debug_plugin, "render_resolution": "1920x1080", "frame_cap": 60,
            "cold_cache_scope": "bbhost only; driver and OS caches are not cleared"}
    if metadata.is_file():
        info["build"] = json.loads(metadata.read_text(encoding="utf-8-sig"))
    (output / "run-info.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
    # A run never inherits a user's bypasses, profiling experiments, paths,
    # account endpoints or capture hooks. GPU/driver system variables remain.
    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith("BBHOST_")}
    base_env.update(BBHOST_SETUP_WINDOW="0", BBHOST_SKIP_INTRO="1", BBHOST_NP_SIGNED_OUT="1",
                    BBHOST_FRAME_STATS="1", BBHOST_STALL_MS="40", BBHOST_GX_RATE="1",
                    BBHOST_EXIT_FLIP=str(args.flips))
    if args.autopress:
        base_env["BBHOST_AUTOPRESS"] = args.autopress
    # No frame dumping, GPU timestamp profiling, screenshots or concurrent run.
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    results = []
    try:
        for index in range(args.runs):
            run = output / ("cold" if index == 0 else "warm-" + str(index))
            data, config = run / "data", run / "config"
            data.mkdir(parents=True); config.mkdir()
            if files:
                destination = data / "saves" / "SPRJ0005"
                destination.mkdir(parents=True)
                for path in files:
                    shutil.copyfile(path, destination / path.name)
            if index:
                cache = data / "bbhost"
                cache.mkdir()
                previous = results[-1]["data"]
                for name in ("stage-manifest.bin", "vulkan-pipeline-cache.bin"):
                    path = Path(previous) / "bbhost" / name
                    if path.is_file():
                        shutil.copyfile(path, cache / name)
            env = dict(base_env, BBHOST_CONFIG_DIR=str(config), BBHOST_OPTIONS_PATH=str(config / "options.toml"))
            toml = config / "bbhost.toml"
            toml.write_text("[paths]\napp0 = " + quote(game) + "\ndata = " + quote(data) +
                            "\nmods = " + quote(data / "mods") +
                            "\n[startup]\nskip_intro = true\nsetup_window = false\n"
                            "[online]\noffline = true\n[update]\ncheck = false\n"
                            "[video]\nwidth = 1920\nheight = 1080\n"
                            "[streaming]\nall_post_processors = false\n"
                            "[plugins]\ndebug_menu = " + ("true" if args.debug_plugin else "false") + "\n",
                            encoding="utf-8")
            (config / "options.toml").write_text('[options]\nresolution = "1920x1080"\n'
                                                'window_mode = "Windowed"\nframe_cap = "60"\n'
                                                'upscaler = "Native / Off"\n', encoding="utf-8")
            # Native one-folder preparation, exact hash gate, local cache only.
            prepared = subprocess.run([str(exe), "--config", str(toml), "--prepare-game", str(game)],
                                      cwd=run, env=env, capture_output=True, timeout=60,
                                      creationflags=flags)
            (run / "preparation.log").write_bytes(prepared.stdout + prepared.stderr)
            result = json.loads(prepared.stdout)
            if prepared.returncode or not result.get("ok"):
                raise RuntimeError("Native game preparation failed; see " + str(run / "preparation.log"))
            start = time.monotonic()
            log = run / "run.log"
            print("Running " + run.name + " with isolated data and saves", flush=True)
            with log.open("wb") as stream:
                child = subprocess.Popen([str(exe), "--config", str(toml)], cwd=run, env=env,
                                         stdout=stream, stderr=subprocess.STDOUT, creationflags=flags)
                try:
                    code = child.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    # Only this child's disposable saves/caches can be affected.
                    child.kill(); child.wait()
                    raise RuntimeError("Timed out; incomplete run excluded: " + str(log))
                except BaseException:
                    if child.poll() is None:
                        child.kill(); child.wait()
                    raise
            item = dict(run=run.name, data=str(data), seconds=round(time.monotonic() - start, 2),
                        exit_code=code, **statistics(log))
            results.append(item)
            (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print(json.dumps({k: v for k, v in item.items() if k not in ("data", "cache_and_load_events")}), flush=True)
            text = log.read_text(encoding="utf-8", errors="replace")
            if code or "exit: flip " + str(args.flips) + " reached" not in text:
                raise RuntimeError("Run did not reach its normal flip-count exit: " + str(log))
    finally:
        unchanged = all(digest(p) == original[p.name] for p in files)
        (output / "source-save-check.json").write_text(json.dumps({"unchanged": unchanged}), encoding="utf-8")
        if not unchanged:
            raise RuntimeError("Source save changed during measurement; comparison is invalid.")
    print("Source saves unchanged. Cold means empty bbhost cache, not empty driver/OS cache.", flush=True)


if __name__ == "__main__":
    main()
