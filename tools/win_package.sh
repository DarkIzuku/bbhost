#!/bin/bash
# Packs the Windows cross build into the zip a Windows machine runs from
# (docs/building.md): the exe without its DWARF, a config template with
# Windows paths, a README, and the empty data/ and build/ directories the
# run writes into. The unstripped exe is kept beside the zip under the same
# revision name, for symbolizing a crash report from that machine.
#   tools/win_package.sh [EXE] [OUTDIR]     (cmake --build build-win --target package-win)
set -eu
cd "$(dirname "$0")/.."
exe=${1:-build-win/bbhost.exe}
out=${2:-build/win}
rev=$(git -c core.excludesFile=/dev/null rev-parse --short HEAD 2>/dev/null || echo unknown)
# A release (HEAD at a v* tag) is named for its version, anything else for its revision.
if tag=$(git -c core.excludesFile=/dev/null describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null); then
    rev=$tag
fi
if [ -n "$(git -c core.excludesFile=/dev/null status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    rev="$rev+"
fi
name=bbhost-win-$rev
dir=$out/$name
rm -rf "$dir"
mkdir -p "$dir/data" "$dir/build"
# The debug menu's font and font shaders (engine/debug_menu.h): the retail
# disc has none, and the debug_menu option refuses without them, so the
# package carries them in its asset overlay (data/mods, ./data in bbhost.toml).
if [ -f "tmp/Debug Menu Restoration.7z" ]; then
    tools/build_debug_assets.sh "tmp/Debug Menu Restoration.7z" "$dir/data/mods/dvdroot_ps4/adhoc" > /dev/null
else
    echo "WARNING: no tmp/Debug Menu Restoration.7z - this package has no debug menu" >&2
fi
cp -r patches "$dir/patches"
mkdir -p "$dir/plugins"
cp include/bbhost_plugin.h "$dir/plugins/"
cp LICENSE "$dir/LICENSE.txt"
tools/package_plugins.sh "$(dirname "$exe")/plugins" "$dir/plugins"
llvm-objcopy --strip-debug "$exe" "$dir/bbhost.exe"
cp "$exe" "$out/$name.debug.exe"
sed -e 's#^app0 = .*#app0 = "C:/Games/CUSA00900"#' \
    -e 's#^eboot = .*#eboot = "./eboot-109-decrypted.bin"#' \
    -e 's#^skip_intro = false#skip_intro = true#' bbhost.example.toml > "$dir/bbhost.example.toml"
cat > "$dir/README.txt" <<EOF
bbhost for Windows, build $name
================================

A native host for the Bloodborne 1.09 eboot (PC, not PS4 emulation).
Free software under the GNU GPL, version 3 or later (LICENSE.txt); the
source is at https://github.com/droogie/bbhost.

You need
  - Windows 10 or 11, x86-64, and a Vulkan 1.3 GPU driver (vulkan-1.dll
    comes with the driver).
  - The eboot.bin of the game's 1.09 update, decrypted to an ELF, as
    eboot-109-decrypted.bin next to bbhost.exe. Its SHA-256 must be
    941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7;
    bbhost stops at start with an explanation when it is not. A dump of
    the game without the update carries the 1.00 eboot, which will not do.
  - The game dump with the 1.09 update's files copied over it: the
    CUSA00900 folder that contains dvdroot_ps4, whose sce_sys/param.sfo
    says APP_VER 01.09.

Setup
  Run bbhost.exe. The first time, the setup window opens: pick the game
  folder (the one that contains dvdroot_ps4) and the decrypted 1.09 eboot
  with the Browse buttons, then Play. bbhost.exe --setup opens it again.
  It writes %APPDATA%\\bbhost\\bbhost.toml (also editable by hand), which
  every copy of bbhost reads, so a new download or an
  update needs nothing set again; the F10 settings and the account live
  beside it. Saves and caches go to %LOCALAPPDATA%\bbhost\data.
  bbhost.example.toml lists every setting.

Run
  From a console, so the log survives the window:
      bbhost.exe 2> bbhost.log
  Double-clicking works too; the log goes to the console that opens.
  F10 in the game window opens the host's options (resolution, window
  mode, vsync, frame cap, keys, account).

Plugins
  plugins/ holds the official ones, signed by the bbhost release key
  (the .sig beside each). Turn them on and set them up in the setup
  window's Plugins tab (bbhost --setup), which can also fetch the latest
  official plugins; in the game, F9 opens the plugin menu (start the boss
  rush there). Or in the per-user bbhost.toml:
      [plugins]
      randomizer = true    # every pickup and shop shuffled from a seed
      boss_rush = true     # the game's bosses back to back, timed
      mutators = true      # speed, bullet time, one-hit, chaos and more
  Each one's settings go in a section of its own name ([randomizer],
  [boss_rush], [mutators]); docs/plugins.md in the repository lists them.
  Use the boss rush on a save of its own.

If it crashes
  The log holds a block starting "SIGSEGV pc=" with the registers, the
  guest address (Binary Ninja's) or the host module+offset, and the
  frames. Send bbhost.log with the build name above; the matching
  $name.debug.exe symbolizes the host addresses:
      addr2line -f -C -e $name.debug.exe 0x<link address>
  If the log stops without that block, the process was ended from
  outside the host (inside the GPU driver, or by Windows): Event Viewer,
  Windows Logs > Application, has an "Application Error" entry with the
  faulting module and exception code - send that with the log.

Switches (environment variables)
  BBHOST_GPU_DEVICE=N      the GPU to use, by the log's "gpu: device N"
                           index or part of its name; the default is a
                           discrete GPU over an on-board one
  BBHOST_SKIP_INTRO=1      skip the company logos (bbhost.toml has it)
  BBHOST_DUMP_FRAME=N      write the displayed frame N as build/frame-N.ppm
  BBHOST_EXIT_FLIP=N       end the run at flip N with the exit reports
  BBHOST_SAMPLE=main       the sampler: build/samples.txt + .maps
  BBHOST_HEADLESS=1        no window
EOF
(cd "$out" && rm -f "$name.zip" && zip -qr "$name.zip" "$name")
echo "package: $out/$name.zip ($(( $(stat -c %s "$out/$name.zip") / 1048576 )) MB); symbols: $out/$name.debug.exe"
