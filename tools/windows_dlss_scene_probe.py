#!/usr/bin/env python3
"""Isolated native scene/temporal diagnostic, with read-only game/save sources.

Log checks prove actual NGX evaluations; they never certify absence of ghosting.
Display captures are for visual review and must not be used for performance timing.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from windows_streaming_probe import digest, quote, seed_files


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime', type=Path, required=True)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--save', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--model-dir', type=Path)
    p.add_argument('--cache-source', type=Path)
    p.add_argument('--resolution', default='1920x1080')
    p.add_argument('--cap', choices=['30', '60', '120', 'Off'], default='60')
    p.add_argument('--flips', type=int, default=1800)
    p.add_argument('--timeout', type=int, default=240)
    p.add_argument('--autopress', required=True, help='Native game test hook; no OS input')
    p.add_argument('--capture-flips', default='', help='Comma-separated presentation flips')
    p.add_argument('--draw-list', type=int, default=0, help='Diagnostic native draw list at capture flips; alters timing')
    p.add_argument('--dump-at-draw', default='', help='Native intermediate target readback: pipeline:occurrence:min-flip')
    p.add_argument('--resize-test', default='', help='Native quiet-point resize hook: flip:WIDTHxHEIGHT[,..]')
    p.add_argument('--scene-dlaa', action='store_true', help='Experimental pre-UI DLAA diagnostic')
    p.add_argument('--clean-effects', action='store_true', help='Disable game AA/blur/DOF/color fringing to isolate temporal artifacts')
    queues = p.add_mutually_exclusive_group()
    queues.add_argument('--single-present-queue', action='store_true', help='Overlay A/B diagnostic; use renderer queue for presentation')
    queues.add_argument('--separate-present-queue', action='store_true', help='Overlay A/B diagnostic; override automatic RTSS compatibility')
    args = p.parse_args()
    runtime, game, seed, root = [v.resolve() for v in (args.runtime, args.game, args.save, args.out)]
    if not re.fullmatch(r'\d{3,5}x\d{3,5}', args.resolution) or args.flips < 1 or args.timeout < 1 or not 0<=args.draw_list<=4096:
        raise ValueError('Invalid resolution, flip count or timeout')
    if args.capture_flips and not re.fullmatch(r'\d+(,\d+)*', args.capture_flips):
        raise ValueError('Capture flips must be a comma-separated list of numbers')
    if args.resize_test and not re.fullmatch(r'\d+:\d{3,5}x\d{3,5}(,\d+:\d{3,5}x\d{3,5})*', args.resize_test):
        raise ValueError('Invalid native resize test sequence')
    if args.dump_at_draw and not re.fullmatch(r'[a-f0-9]+(?:\+[a-f0-9]+)?:\d+:\d+', args.dump_at_draw):
        raise ValueError('Invalid native pipeline readback selector')
    if args.scene_dlaa and (not args.model_dir or not args.model_dir.is_dir()):
        raise ValueError('DLAA requires the local model directory')
    for protected in (runtime, game, seed, args.cache_source, args.model_dir):
        if protected:
            protected = protected.resolve()
            if root == protected or protected in root.parents or root in protected.parents:
                raise ValueError('Output must be separate from every read-only source')
    if not (runtime/'bbhost.exe').is_file() or not game.is_dir():
        raise ValueError('Runtime or game folder missing')
    files = seed_files(seed)
    original = {f.name: digest(f) for f in files}
    root.mkdir(parents=True, exist_ok=False)
    config, data = root/'config', root/'data'
    config.mkdir(); (root/'build').mkdir(); (data/'saves/SPRJ0005').mkdir(parents=True)
    for f in files:
        shutil.copyfile(f, data/'saves/SPRJ0005'/f.name)
    if args.cache_source:
        (data/'bbhost').mkdir()
        for name in ('stage-manifest.bin', 'vulkan-pipeline-cache.bin'):
            if (args.cache_source/name).is_file():
                shutil.copyfile(args.cache_source/name, data/'bbhost'/name)
    toml = config/'bbhost.toml'
    toml.write_text('[paths]\napp0 = '+quote(game)+'\ndata = '+quote(data)+'\nmods = '+quote(data/'mods')+
                    '\n[startup]\nsetup_window = false\nskip_intro = true\n[online]\noffline = true\n'
                    '[update]\ncheck = false\n[plugins]\ndebug_menu = false\n', encoding='utf-8')
    options = config/'options.toml'
    options.write_text('[options]\nresolution = '+json.dumps(args.resolution)+'\nwindow_mode = "Windowed"\nframe_cap = '+
                       json.dumps(args.cap)+'\nupscaler = "Native / Off"\n', encoding='utf-8')
    if args.clean_effects:
        with options.open('a',encoding='utf-8') as file:
            file.write('motion_blur = "Off"\ndepth_of_field = "Off"\nchromatic_aberration = "Off"\nanti_alias = "Off"\n')
    env = {k:v for k,v in os.environ.items() if not k.upper().startswith('BBHOST_')}
    env.update(BBHOST_CONFIG_DIR=str(config), BBHOST_OPTIONS_PATH=str(options), BBHOST_SETUP_WINDOW='0',
               BBHOST_NP_SIGNED_OUT='1', BBHOST_SKIP_INTRO='1', BBHOST_EXIT_FLIP=str(args.flips),
               BBHOST_AUTOPRESS=args.autopress, BBHOST_TEMPORAL_AUDIT='1')
    if args.scene_dlaa:
        env.update(BBHOST_DLSS_SCENE='1', BBHOST_DLSS_MODEL_PATH=str(args.model_dir.resolve()), BBHOST_DLSS_LOG='1')
    if args.capture_flips:
        env['BBHOST_DUMP_FRAME'] = args.capture_flips
    if args.draw_list:
        env['BBHOST_DUMP_DRAWS'] = str(args.draw_list)
    if args.resize_test:
        env['BBHOST_RESIZE_TEST'] = args.resize_test
    if args.dump_at_draw:
        env['BBHOST_DUMP_AT_DRAW'] = args.dump_at_draw
    if args.single_present_queue:
        env['BBHOST_PRESENT_QUEUE'] = '0'
    elif args.separate_present_queue:
        env['BBHOST_PRESENT_QUEUE'] = '1'
    log_path = root/'run.log'
    code = None
    try:
        with log_path.open('wb') as log:
            child = subprocess.Popen([str(runtime/'bbhost.exe'), '--config', str(toml)], cwd=root, env=env,
                                     stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                code = child.wait(timeout=args.timeout)
            finally:
                if child.poll() is None:
                    child.kill(); child.wait()
    finally:
        if any(digest(f) != original[f.name] for f in files):
            raise RuntimeError('Original save changed')
    text = log_path.read_text(encoding='utf-8', errors='replace')
    resolved = [int(n) for n in re.findall(r'temporal-boundary: GX flip=(\d+)[^\n]*resolve=DLAA', text)]
    versions = sorted(set(re.findall(r'loaded model file version=([^;\s]+)', text)))
    totals = re.findall(r'DLSS: scene evaluations=(\d+); history resets=(\d+)',text)
    temporal_failures = [line for line in text.splitlines() if
                         'scene ended without a verified temporal resolve' in line or
                         ('temporal-boundary:' in line and 'jittered-CBs=0' not in line and
                          'resolve=audit/native' in line)]
    result = dict(exit_code=code, original_saves_unchanged=True, scene_dlaa=args.scene_dlaa,
                  logged_scene_evaluations=len(resolved), logged_scene_frames=resolved, loaded_model_versions=versions,
                  captures=[f.name for f in sorted((root/'build').glob('frame-*'))],
                  temporal_failures=temporal_failures,
                  clean_effects=args.clean_effects,
                  single_present_queue=args.single_present_queue,
                  separate_present_queue=args.separate_present_queue,
                  resize_test=args.resize_test,
                  intermediate_readback=args.dump_at_draw,
                  intermediate_images=[f.name for f in sorted((root/'build').glob('rtd-*'))],
                  applied_resizes=re.findall(r'resolution: (\d+x\d+) -> (\d+x\d+) in', text),
                  scene_evaluations_total=int(totals[-1][0]) if totals else None,
                  history_resets=int(totals[-1][1]) if totals else None,
                  visual_ghosting_review='pending', performance_measurement=False)
    (root/'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    if code != 0:
        raise RuntimeError('Native game exited with '+str(code))
    if args.dump_at_draw and not result['intermediate_images']:
        raise RuntimeError('Requested intermediate pipeline readback was not reached')
    if args.scene_dlaa and len(resolved) < 4:
        raise RuntimeError('Too few actual scene evaluations: NGX initialization alone is not a pass')
    if args.scene_dlaa and temporal_failures:
        raise RuntimeError('Temporal scene evaluation fell back during the run; not a DLSS pass')
    if args.scene_dlaa and (not totals or int(totals[-1][0]) < 64):
        raise RuntimeError('At least 64 real scene evaluations and an evaluation summary are required')
    summary = dict(result)
    summary['intermediate_image_count'] = len(summary.pop('intermediate_images'))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
