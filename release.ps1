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

# ⚠️ Not a hand-typed version by default. One that does not match the binary is
# worse than none: it names the wrong build with total confidence.
#
# DS5-USBIP's own version (2026-09-16): read from include/ctm/product.h, the one
# place it is written, instead of the exe's timestamp. That constant is what the
# window title, --version and the exe's product fields show too, so the zip and
# the program cannot disagree. The zip and its folder carry the product's name.
if (-not $Version) {
    $productHeader = Join-Path $Root 'include\ctm\product.h'
    $match = Select-String -Path $productHeader -Pattern '#define PRODUCT_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"'
    if (-not $match) { throw "no PRODUCT_VERSION in $productHeader" }
    $Version = $match.Matches[0].Groups[1].Value
}

$stage = Join-Path $Root "out\release\DS5-USBIP-$Version"

# ⛔⛔ THE FOLDER THIS STAGES INTO MAY BE A COPY SOMEBODY RUNS, WITH THEIR
# CONFIGS IN IT. The exe keeps its configs, its log and its window's place
# beside itself, so a release folder that has been started once is a home.
# This script used to delete the folder and stage afresh, and on 2026-10-02
# that took a config made in it two hours before. Nothing said so: it showed
# only in a listing taken beforehand for another reason.
#
# ➡️ So the old folder is set ASIDE rather than deleted, the new one is staged
# and zipped CLEAN, and then everything the release does not ship goes back in.
#
# ⓘ After the zip, never before. The zip is what gets published, and a log or
# a config in it would publish a person's controller serials and their paths.
# ⓘ "Everything the release does not ship", not a list of the files the exe is
# known to write. A list goes stale the day the exe writes one more, and what
# that costs is somebody's settings. What this rule costs is a file an older
# release shipped lingering beside the new ones, and the known ones are named.
# ⓘ A release that FAILS puts the folder back exactly as it found it (the trap
# below). A half-staged folder with no exe in it is somebody's shortcut that
# no longer starts.
$aside = "$stage.previous"
$retired = @('start-ctm-usbip.bat')    # shipped once and not any more: not put back
$staging = $false

# ⛔ Not while it is running from there. Windows will not let a running exe be
# replaced, and finding that out half way through is how a folder gets left in
# pieces.
$running = @(Get-CimInstance Win32_Process -Filter "Name='ctm-usbip.exe'" |
             Where-Object { $_.ExecutablePath -and
                            $_.ExecutablePath.StartsWith("$stage\", [System.StringComparison]::OrdinalIgnoreCase) })
if ($running.Count -gt 0) {
    throw "DS5-USBIP is running from $stage -- choose Quit from its tray icon, then run this again"
}
# ⛔ Never deleted to get out of the way: it is the only copy of whatever an
# earlier run had set aside and did not get to put back.
if ((Test-Path -LiteralPath $aside) -and (Test-Path -LiteralPath $stage)) {
    throw "an earlier run stopped part way, and what it had set aside is still in $aside -- move anything you want to keep from there into $stage, remove $aside, then run this again"
}

trap {
    # ⓘ Only between the folder being set aside and the zip being made. Before
    # that nothing has been touched; after it the person's files are on their
    # way back in, and undoing would take them along.
    if ($staging) {
        Write-Host ''
        Write-Host "release FAILED -- putting $stage back as it was" -ForegroundColor Red
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
        if (Test-Path -LiteralPath $aside) { Move-Item -LiteralPath $aside -Destination $stage }
    }
    break
}

# ⓘ One rename, so it either happens or it does not. $staging is set AFTER it:
# were the rename to fail, the trap must not take the folder for a new one.
if (Test-Path -LiteralPath $stage) { Move-Item -LiteralPath $stage -Destination $aside }
$staging = $true
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

# ⭐ NO LAUNCHER. There was one, start-ctm-usbip.bat, because a double-click
# on the exe passed no arguments, printed the usage and exited: a window
# flashed and vanished, which read as a crash. The exe is a Windows program
# now and a double-click IS how it is started, so the script that stood in
# front of it is gone.

# The desktop-shortcut generator, both halves. The .bat is what the user
# double-clicks, because Windows opens a double-clicked .ps1 in Notepad rather
# than running it.
# ⛔ Throws rather than copying quietly. A silent Copy-Item once shipped a zip
# with a file missing and nothing said so; the same reason profiles and maps
# are checked below.
foreach ($shortcutPart in @('tools\create-desktop-shortcut.bat',
                            'tools\create-desktop-shortcut.ps1')) {
    $shortcutFile = Join-Path $Root $shortcutPart
    if (-not (Test-Path $shortcutFile)) { throw "shortcut generator missing: $shortcutFile" }
    Copy-Item -Force -Path $shortcutFile -Destination $stage
}

# A README in the zip, because the first question is always how to start it.
$readme = @"
DS5-USBIP $Version
==================

Start it: double-click ctm-usbip.exe

It runs in the background. No window opens for it: look for the DS5-USBIP icon
in the tray, by the clock. Click it for the settings page or the on-screen
keyboard, and choose Quit there to close it. Double-click the exe again while
it is running and the settings page comes to the front.

Want it on your desktop? Double-click create-desktop-shortcut.bat once.
It builds the shortcut from wherever this folder currently is, so MOVE THE
FOLDER FIRST and then run it -- a Windows shortcut stores the full path and
cannot follow the folder afterwards. Running it again replaces the old one.

To watch it instead, with its log on screen, type this at a command prompt
in this folder. The prompt comes straight back and the listener's lines follow
in the same window. Ctrl+C in that window ends it, and so does closing the
window or choosing Quit from the tray icon:

    ctm-usbip.exe agent --ui --verbose

IMPORTANT: FIRST, install usbip-win2 0.9.77 from
https://github.com/vadimgrn/usbip-win2/releases -- a separate project whose
signed driver lets Windows attach the controller. Without it this listener
starts normally and bridges nothing.

Keep this folder together, somewhere writable -- Desktop or Documents, not
Program Files. The exe finds profiles and maps beside itself, and creates its
config and logs here as you use it, wherever it is started from. If the
profiles folder is missing it says so and does not start.

--ui opens the settings page in a browser window. The page is built into the
exe and served by it, so there is no file to place and it cannot fall out of
step with the version you are running.

What is in here, and what each part is for:

    ctm-usbip.exe             the listener: double-click it
    create-desktop-shortcut.bat   puts a shortcut to it on your desktop
    *.dll                     ffmpeg, for audio
    profiles\descriptors\     what each controller looks like over USB
    maps\                     how its reports translate

Profiles and maps are DATA, on purpose. Adding a controller means adding a
file here, not rebuilding -- so keep the folders beside the exe. Without them
the listener says the profiles folder is missing and does not start.

Two files are created next to the exe as you use it:

    ctm-device-config.txt     settings shared by every controller
    configs\                  per-controller settings, made from the page

Full documentation: https://github.com/rhoquinn8217/CTM-USBIP
"@
[System.IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme,
                               (New-Object System.Text.UTF8Encoding($false)))

$zip = Join-Path $Root "out\release\DS5-USBIP-$Version.zip"
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

# ⭐ The zip is made and listed. Now what the person had goes back into the
# folder: everything that was in it and is not something this release ships.
# ⛔ From here a failure undoes NOTHING. Whatever has not been put back yet is
# still in the set-aside folder, and the next run will refuse to delete it.
$staging = $false
if (Test-Path -LiteralPath $aside) {
    $kept = New-Object System.Collections.Generic.List[string]
    foreach ($file in @(Get-ChildItem -LiteralPath $aside -Recurse -File -Force)) {
        $inFolder = $file.FullName.Substring($aside.Length + 1)
        if ($retired -contains $inFolder) { continue }
        $back = Join-Path $stage $inFolder
        if (Test-Path -LiteralPath $back) { continue }     # shipped: the new one stands
        $backDir = Split-Path -Parent $back
        if (-not (Test-Path -LiteralPath $backDir)) { New-Item -ItemType Directory -Force -Path $backDir | Out-Null }
        Move-Item -LiteralPath $file.FullName -Destination $back
        $kept.Add($inFolder)
    }
    # What is left in there is the previous release's own files, superseded.
    Remove-Item -LiteralPath $aside -Recurse -Force
    if ($kept.Count -gt 0) {
        Write-Host ''
        Write-Host 'kept from the folder that was here, and NOT in the zip:' -ForegroundColor Green
        foreach ($name in $kept) { Write-Host "  $name" }
    }
}
