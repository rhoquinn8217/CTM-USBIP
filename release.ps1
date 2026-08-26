# Assembles a release zip.
#
# ⭐ WHAT GOES IN AND WHY IT IS NOT JUST THE EXE.
#
# The settings page IS embedded -- a user should not have to place an HTML file
# correctly, and a page that can drift from the API it talks to is a bug waiting
# to happen. Profiles and maps are NOT, and that is deliberate: this project's
# own rule is that adding a device is data, never a recompile. Baking them into
# the binary would make adding a controller a build.
#
# ⛔ Found the hard way on 2026-08-23: an exe copied on its own bridged nothing,
# failing with "Could not open descriptor profile". Embedding the page had made
# it look like one file was enough.

param(
    [string]$Configuration = 'Debug',
    [string]$Platform = 'x64',
    [string]$Version = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binaries = Join-Path $Root "out\$Platform\$Configuration"

if (-not (Test-Path (Join-Path $binaries 'ctm-usbip.exe'))) {
    throw "ctm-usbip.exe not found in $binaries -- run build.ps1 first"
}

# ⚠️ Version from the exe rather than a parameter by default. A hand-typed
# version that does not match the binary is worse than none: it names the wrong
# build with total confidence.
if (-not $Version) {
    $Version = (Get-Item (Join-Path $binaries 'ctm-usbip.exe')).LastWriteTime.ToString('yyyyMMdd-HHmm')
}

$stage = Join-Path $Root "out\release\ctm-usbip-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'profiles\descriptors') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'maps') | Out-Null

Copy-Item -Force -Path (Join-Path $binaries 'ctm-usbip.exe') -Destination $stage
Copy-Item -Force -Path (Join-Path $binaries '*.dll') -Destination $stage

# ⛔ Copied from the REPO, not from the build output. The build output is a
# staging area that accumulates whatever ran there -- a profile someone dropped
# in by hand to test something would ship. The repo is the source of truth.
Copy-Item -Force -Path (Join-Path $Root 'profiles\descriptors\*.profile') `
                 -Destination (Join-Path $stage 'profiles\descriptors')
Copy-Item -Force -Path (Join-Path $Root 'maps\*.map') -Destination (Join-Path $stage 'maps')

# ⭐ The launcher. Double-clicking the exe passes no arguments, so it prints
# usage and exits -- a window flashes and vanishes, which reads as a crash.
#
# ⛔ Throws rather than copying quietly. A silent Copy-Item shipped a zip with
# no launcher in it and nothing said so; the same reason profiles and maps are
# checked below.
$launcher = Join-Path $Root 'tools\start-ctm-usbip.bat'
if (-not (Test-Path $launcher)) { throw "launcher missing: $launcher" }
Copy-Item -Force -Path $launcher -Destination $stage

# A README in the zip, because the first question is always how to start it.
$readme = @"
CTM-USBIP $Version
==================

Start it: double-click start-ctm-usbip.bat

Or from a command prompt in this folder:

    ctm-usbip.exe agent 48054 --ui

IMPORTANT: FIRST, install usbip-win2 0.9.77 from
https://github.com/vadimgrn/usbip-win2/releases -- a separate project whose
signed driver lets Windows attach the controller. Without it this listener
starts normally and bridges nothing.

Keep this folder together, somewhere writable -- Desktop or Documents, not
Program Files. The exe finds profiles and maps beside itself, and creates its
config and logs here as you use it.

--ui opens the settings page in a browser window. The page is built into the
exe and served by it, so there is no file to place and it cannot fall out of
step with the version you are running.

What is in here, and what each part is for:

    ctm-usbip.exe             the listener
    *.dll                     ffmpeg, for audio
    profiles\descriptors\     what each controller looks like over USB
    maps\                     how its reports translate

Profiles and maps are DATA, on purpose. Adding a controller means adding a
file here, not rebuilding -- so keep the folders beside the exe. Without them
the listener starts but cannot bridge anything, and says
"Could not open descriptor profile".

Two files are created next to the exe as you use it:

    ctm-device-config.txt     settings shared by every controller
    configs\                  per-controller settings, made from the page

Full documentation: https://github.com/rhoquinn8217/CTM-USBIP
"@
[System.IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme,
                               (New-Object System.Text.UTF8Encoding($false)))

$zip = Join-Path $Root "out\release\ctm-usbip-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# ⭐ The FOLDER, not its contents. Extracting loose files into whatever
# directory the user picked scatters them among what is already there -- and
# these must stay together: the exe finds profiles and maps beside itself.
# ZipFile rather than Compress-Archive, for the includeBaseDirectory argument
# below.
#
# NOTE: this does NOT fix the "appears to use backslashes as path separators"
# warning -- tested 2026-08-26, ZipFile writes the platform separator too.
# Fixing it properly means enumerating files and writing entries by hand, which
# is real code and real risk for a cosmetic message every extractor tolerates.
# Left alone deliberately.
#
# The last argument is includeBaseDirectory: true puts everything inside one
# top-level folder, so extracting does not scatter files into whatever
# directory the user picked -- and these must stay together, since the exe
# finds profiles and maps beside itself.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)

# ⭐ Report what shipped rather than just "done". A release missing a profile
# starts fine and fails only when someone tries to bridge, which is a long way
# from here.
Write-Host ''
Write-Host "release: $zip" -ForegroundColor Green
Get-ChildItem -Recurse -File $stage | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1)
    Write-Host ("  {0,-42} {1,8:N0} bytes" -f $rel, $_.Length)
}
$profiles = (Get-ChildItem (Join-Path $stage 'profiles\descriptors') -File).Count
$maps = (Get-ChildItem (Join-Path $stage 'maps') -File).Count
Write-Host ''
Write-Host "$profiles profile(s), $maps map(s)" -ForegroundColor DarkGray
if ($profiles -eq 0 -or $maps -eq 0) {
    throw 'no profiles or no maps were staged -- the listener could not bridge anything'
}
