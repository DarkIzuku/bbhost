# Running bbhost

## What you need

bbhost contains no game data. You bring:

1. **The game files**: a dump of Bloodborne with the 1.09 update's files copied
   over it - the folder that contains `dvdroot_ps4`. Its
   `sce_sys/param.sfo` says `APP_VER 01.09`.
2. **The 1.09 eboot**, decrypted to an ELF. Its SHA-256 must be
   `941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7`. Every
   patch, hook and engine layout in bbhost is an address in that build, so any
   other eboot stops at start with its hash and the version its dump reports.
   A dump without the update carries the 1.00 eboot, which will not do.

Both must come from a copy of the game you own.

And a PC with:

- x86-64 Windows 10/11 or Linux
- a GPU with a Vulkan 1.3 driver (recent NVIDIA, AMD or Intel; the Steam Deck works)
- 16 GB of RAM and 8 GB of video memory are recommended

## First run

Started without a configuration, bbhost opens its setup window: pick the game
folder and the eboot (both are checked as you go), choose the online server
and account if you want to play online, and press Play. The window writes the
per-user `bbhost.toml` and starts the game. `bbhost --setup` opens it again.

Without a display (a headless run) bbhost writes a template config instead and
tells you which paths to fill in.

## Configuration

`bbhost.example.toml` lists every setting with its default. The configuration
is read in layers, each overriding only the keys it sets:

1. the per-user `bbhost.toml`: `%APPDATA%\bbhost` on Windows,
   `$XDG_CONFIG_HOME/bbhost` or `~/.config/bbhost` on Linux
   (`BBHOST_CONFIG_DIR` replaces the folder);
2. `bbhost.toml` in the working directory, or next to the executable;
3. `--config <file>`;
4. the command line: `--app0 <dir>`, `--data <dir>`, `--eboot <file>`, or the
   eboot as the last argument.

The first log line names every file it read. Paths may use either slash; a
relative path is taken from the folder of the file it is in.

The in-game settings (F10, or the PC Settings, PC Graphics and Key Bindings
entries the game's own System menu gains) are saved beside the per-user config
in `bbhost-options.toml`.

## Saves

Saves are in `<data>/saves/SPRJ0005`, where `<data>` defaults to
`%LOCALAPPDATA%\bbhost\data` or `~/.local/share/bbhost/data`. Each time the
game writes a save (at most every five minutes) a copy goes to
`<data>/save-backups/`, and the newest eight are kept. To restore one, quit and
copy its files over `<data>/saves/SPRJ0005`.

## Controls

A controller works as on the console. Without one, the keyboard and mouse
take over and the button prompts follow: the mouse turns the camera and works
in the menus, and every action can be rebound (F10 or the game's Key Bindings
menu). F9 opens the plugin menu and F10 bbhost's own settings.

## Logs

bbhost logs to standard error. On Windows, run it from a console to keep the
log (`bbhost.exe 2> bbhost.log`); the Linux package's `run-bbhost.sh` writes
each session's log to `logs/`. A bug report is most useful with that log
attached.

## Troubleshooting

**The eboot is refused.** It is not the 1.09 build. Use the update's
`eboot.bin`, decrypted; the message says what the dump's `param.sfo` reports.

**The graphics card stops responding.** When the GPU hangs and the system
resets it, nothing can be drawn again in that session; bbhost stops the sound,
says so in a box and exits. The next three starts run with the GPU's progress
markers enabled, so the log of a second hang names the draw that hung - send
that log. Overlays that hook Vulkan are a common cause: OBS's game capture is
kept out automatically on AMD cards (`[video] obs_capture` changes that), and
OBS's Window Capture records bbhost without any hook.

**Black or broken image in the world.** On cards with little video memory,
check the log for `video memory is full`. bbhost sizes its own buffers to the
card's budget and falls back to system memory rather than failing, but other
programs holding video memory reduce what is left.

**AMD on Linux, no picture.** AMD's kernel driver only imports anonymous
memory, so bbhost passes the game's memory to the GPU through `/dev/udmabuf`.
If that device is not accessible to your user, the log says so; this udev rule
fixes it:

```
# /etc/udev/rules.d/70-udmabuf.rules
KERNEL=="udmabuf", TAG+="uaccess"
```

**Steam Deck.** Start bbhost from Steam (as a non-Steam game), in Game Mode or
Desktop Mode; otherwise Steam's desktop controls send keys alongside the pad
and every press acts twice. `tools/steamdeck/STEAMDECK.md` has the setup.

## Display and frame rate

The picture keeps its aspect ratio, with ultrawide resolutions (21:9, 32:9)
available in the settings. The resolution can be changed while playing.
bbhost paces the game to the refresh rate of the display its window is on;
`[video] vblank_hz` overrides a display that reports the wrong rate. The frame
cap (30, 60 or higher) is an in-game setting.

## Online

bbhost implements the PlayStation Network calls the game makes against a
private server, so co-op, invasions, messages and bloodstains work between
bbhost players. The setup window picks the server and links an account; see
[online.md](online.md).
