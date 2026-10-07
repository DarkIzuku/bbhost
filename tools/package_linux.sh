#!/bin/bash
# Packs a Linux build into the tarball another Linux machine runs from, the
# counterpart of tools/win_package.sh: the binary without its DWARF, the patch
# manifests it applies (patches/, looked for next to the binary), the plugin
# header, the example config, a README with what the machine must have, a
# launcher that keeps a log file, and the empty data/, logs/ and build/ (frame
# dumps) the run writes into. The unstripped binary is kept beside the tarball under the same
# revision name, for symbolizing a crash report from that machine.
#   tools/package_linux.sh [BIN] [OUTDIR]
# BIN defaults to build-linux-portable/build/bbhost (tools/linux_portable_build.sh),
# the build that runs on other distributions; build/bbhost only runs on a
# machine as new as this one. Never packs game files (eboot, dump, keys).
set -eu
cd "$(dirname "$0")/.."
bin=${1:-build-linux-portable/build/bbhost}
out=${2:-build/linux}
[ -x "$bin" ] || { echo "no binary at $bin (tools/linux_portable_build.sh builds it)"; exit 1; }
rev=$(git -c core.excludesFile=/dev/null rev-parse --short HEAD 2>/dev/null || echo unknown)
# A release (HEAD at a v* tag) is named for its version, anything else for its revision.
if tag=$(git -c core.excludesFile=/dev/null describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null); then
    rev=$tag
fi
if [ -n "$(git -c core.excludesFile=/dev/null status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    rev="$rev+"
fi
name=bbhost-linux-$rev
dir=$out/$name
rm -rf "$dir"
mkdir -p "$dir/data" "$dir/logs" "$dir/plugins" "$dir/build"
# The debug menu's font and font shaders (engine/debug_menu.h): the retail
# disc has none, and the debug_menu option refuses without them, so the
# package carries them in its asset overlay (data/mods, ./data in bbhost.toml).
if [ -f "tmp/Debug Menu Restoration.7z" ]; then
    tools/build_debug_assets.sh "tmp/Debug Menu Restoration.7z" "$dir/data/mods/dvdroot_ps4/adhoc" > /dev/null
else
    echo "WARNING: no tmp/Debug Menu Restoration.7z - this package has no debug menu" >&2
fi
cp -r patches "$dir/patches"
cp include/bbhost_plugin.h "$dir/plugins/"
cp LICENSE "$dir/LICENSE"
tools/package_plugins.sh "$(dirname "$bin")/plugins" "$dir/plugins"
objcopy --strip-debug "$bin" "$dir/bbhost" 2>/dev/null || llvm-objcopy --strip-debug "$bin" "$dir/bbhost"
chmod +x "$dir/bbhost"
cp "$bin" "$out/$name.debug"
cp bbhost.example.toml "$dir/bbhost.example.toml"

# What the binary still loads from the system, from its own dynamic section,
# so the README cannot drift from the build.
needed=$(readelf -d "$bin" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')
glibc=$(objdump -T "$bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
glibcxx=$(objdump -T "$bin" | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1)

cat > "$dir/run-bbhost.sh" <<'EOF'
#!/bin/bash
# Starts bbhost and keeps its log in logs/ (the newest ten are kept).
#   ./run-bbhost.sh                       the per-user config (~/.config/bbhost/bbhost.toml)
#   BBHOST_CONFIG=other.toml ./run-bbhost.sh   that file on top of it
cd "$(dirname "$0")" || exit 1
cfg=${BBHOST_CONFIG:-bbhost.toml}
mkdir -p logs
log=logs/bbhost-$(date +%Y%m%d-%H%M%S).log
ls -1t logs/bbhost-*.log 2>/dev/null | tail -n +10 | xargs -r rm -f
args=()
[ -f "$cfg" ] && args=(--config "$cfg")
echo "bbhost: config ${args[1]:-per-user}, log $(pwd)/$log"
# tee -i: Ctrl-C stops the game, not the log, so its last lines are kept.
./bbhost "${args[@]}" "$@" 2>&1 | tee -i "$log"
exit "${PIPESTATUS[0]}"
EOF
chmod +x "$dir/run-bbhost.sh"

cat > "$dir/README-linux.txt" <<EOF
bbhost for Linux, build $name
==================================

A native host for the Bloodborne 1.09 eboot (PC, not PS4 emulation).
Free software under the GNU GPL, version 3 or later (LICENSE); the source
is at https://github.com/droogie/bbhost.

You need
  - x86-64 Linux with glibc ${glibc#GLIBC_} or newer (Ubuntu 24.04, Linux Mint 22, Debian 13,
    Fedora 39 or anything newer; Arch; a current SteamOS). Check with: ldd --version
  - A Vulkan 1.3 GPU driver and the Vulkan loader. Check with: vulkaninfo --summary
  - These system libraries (every desktop has most of them already):
        $needed
    Packages that provide them:
        Ubuntu / Debian / Mint : libvulkan1 mesa-vulkan-drivers libcurl4t64 zlib1g
        Fedora                 : vulkan-loader mesa-vulkan-drivers libcurl zlib
        Arch / SteamOS         : vulkan-icd-loader vulkan-radeon|vulkan-intel|nvidia-utils curl zlib
    NVIDIA: the proprietary driver brings its own Vulkan driver.
  - X11 or Wayland, and PulseAudio, PipeWire or ALSA for sound (SDL3 is built
    in and uses what it finds).
  - The eboot.bin of the game's 1.09 update, decrypted to an ELF (SHA-256
    941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7; bbhost
    stops at start and says why when it is another version), and the game
    dump with the 1.09 update's files copied over it: the CUSA00900 folder
    that contains dvdroot_ps4, whose sce_sys/param.sfo says APP_VER 01.09.
    Neither is in this package.

Built static: libstdc++ ($glibcxx), SDL3, ffmpeg (movie decoders only),
SPIRV-Tools.

Setup
  Run ./run-bbhost.sh once: it writes ~/.config/bbhost/bbhost.toml and says so.
  Set app0 and eboot in that file. Every copy of bbhost reads it, so a new
  download or an update needs nothing set again; the F10 settings and the
  account live beside it, saves and caches in ~/.local/share/bbhost/data.
  bbhost.example.toml lists every setting.

Run
  ./run-bbhost.sh          (the log goes to logs/ and the terminal)
  F10 in the game window opens the host's options (resolution, window mode,
  vsync, frame cap, keys, account).

Steam Deck
  The release's bbhost-steamdeck tarball is this package with the Deck's
  settings (its screen's 1280x800, full screen, 30 fps) and STEAMDECK.md,
  the steps for adding it to Steam and playing in Game Mode.

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
  The log holds a block starting "SIGSEGV pc=" with the registers and the
  frames. Send the log with the build name above; the matching
  $name.debug (kept by whoever built this) symbolizes it.
EOF

# Packed from a local copy with plain modes: on a filesystem without them
# (NFS here) every file would go out executable.
stage=$(mktemp -d); trap 'rm -rf "$stage"' EXIT
cp -r "$dir" "$stage/"
find "$stage" -type d -exec chmod 755 {} + && find "$stage" -type f -exec chmod 644 {} +
chmod 755 "$stage/$name/bbhost" "$stage/$name/run-bbhost.sh"
rm -f "$out/$name.tar.gz"
tar czf "$out/$name.tar.gz" -C "$stage" --owner=0 --group=0 "$name"
echo "package: $out/$name.tar.gz ($(( $(stat -c %s "$out/$name.tar.gz") / 1048576 )) MB); symbols: $out/$name.debug"
