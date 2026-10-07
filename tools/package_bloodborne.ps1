param(
    [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
    [Parameter(Mandatory = $true)][string]$LauncherDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{7,40}$')][string]$Commit
)
$ErrorActionPreference = 'Stop'
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$launcher = (Resolve-Path -LiteralPath $LauncherDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$name = 'Bloodborne-PC-Windows-' + $Commit.Substring(0, 7)
$package = Join-Path $output $name
if (Test-Path -LiteralPath $package) { throw "Package directory already exists: $package" }
$archives = @(Get-ChildItem -LiteralPath $runtime -Filter 'bbhost-win-*.zip')
if ($archives.Count -ne 1) { throw 'Exactly one native runtime archive is required.' }
$unpacked = Join-Path $output ('runtime-' + [Guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $archives[0].FullName -DestinationPath $unpacked
$native = @(Get-ChildItem -LiteralPath $unpacked -Directory)
if ($native.Count -ne 1 -or !(Test-Path -LiteralPath (Join-Path $native[0].FullName 'bbhost.exe'))) { throw 'Incomplete native runtime.' }
if (!(Test-Path -LiteralPath (Join-Path $launcher 'BloodborneLauncher.exe'))) { throw 'Missing WPF launcher.' }
New-Item -ItemType Directory -Path $package | Out-Null
foreach ($item in Get-ChildItem -LiteralPath $native[0].FullName) {
    if ($item.Name -notin @('run-bbhost.bat', 'README.txt', 'bbhost.example.toml')) { Copy-Item -LiteralPath $item.FullName -Destination $package -Recurse }
}
Copy-Item -LiteralPath (Join-Path $launcher 'BloodborneLauncher.exe') -Destination $package
foreach ($dll in Get-ChildItem -LiteralPath $launcher -Filter '*.dll') { Copy-Item -LiteralPath $dll.FullName -Destination $package }
foreach ($folder in @('mods', 'logs', 'plugins', 'patches')) { New-Item -ItemType Directory -Path (Join-Path $package $folder) -Force | Out-Null }
Copy-Item -LiteralPath (Join-Path $runtime 'build-info.json') -Destination $package
@'
# Optional native configuration example. BloodborneLauncher writes your real
# config to %APPDATA%/bbhost, shared with bbhost and the in-game PC menus.
[paths]
app0 = ""
eboot = "" # internal; automatically prepared from app0

[startup]
setup_window = false

[online]
offline = true

[update]
check = false
'@ | Set-Content -LiteralPath (Join-Path $package 'bbhost.example.toml') -Encoding utf8
@"
Bloodborne PC - bbhost / WPF integration build $Commit

1. Extract this complete folder to a writable location.
2. Open BloodborneLauncher.exe.
3. Select your complete Bloodborne 1.09 game folder.
4. Choose graphics options and press JUGAR BLOODBORNE.

The executable is found and prepared automatically in bbhost's data cache.
No separate executable selection is required. Unknown versions and encrypted
or compressed SELF inputs are rejected; this build does not decrypt them.
No game files are included in this package.

The launcher disappears during play and returns when the game exits, including
after a crash. No command window is needed. Session logs are in logs/ (ten
recent sessions); Detailed Logs and a developer console are optional.

Settings: %APPDATA%/bbhost. Default saves/cache: %LOCALAPPDATA%/bbhost/data.
The game folder and old saves are never modified by automatic preparation.
Native save backups remain enabled. Import refuses an existing destination.

Available upscaling: Native / Off and upstream FSR 1. Temporal FSR, DLSS and
DLAA are pending integration and are not selectable in this build.

Online starts Offline. Custom servers use separate native profiles. Discord
and Hunter's Dream presets are removed. shadNet needs a protocol adapter;
its current binary account/matching API cannot replace bbhost's JSON API.

F10 opens native PC options; F9 opens native plugin options during gameplay.
Keep the entire plugins/, patches/ and data/ folders from this package.
Source: https://github.com/DarkIzuku/bbhost/tree/bloodborne-pc-integration-v1
Base: https://github.com/droogie/bbhost
License: LICENSE.txt. Build/toolchain facts: build-info.json.
"@ | Set-Content -LiteralPath (Join-Path $package 'README.txt') -Encoding utf8
$archive = Join-Path $output ($name + '.zip')
Compress-Archive -LiteralPath $package -DestinationPath $archive -CompressionLevel Optimal
Get-FileHash -LiteralPath $archive -Algorithm SHA256
