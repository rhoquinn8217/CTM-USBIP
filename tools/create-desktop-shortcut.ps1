# Puts a DS5-USBIP shortcut on the desktop, pointing at THIS folder.
#
# Run it by double-clicking create-desktop-shortcut.bat beside this file.
# Nothing to configure and nothing to type.
#
# ---------------------------------------------------------------------------
# WHY THIS SCRIPT IS PURE ASCII, DELIBERATELY.
#
# Windows PowerShell 5.1 reads a file with no byte-order mark as ANSI, not as
# UTF-8. A comment glyph would come out as mojibake, and a non-ASCII character
# inside a STRING would reach the desktop in a shortcut name. The rest of this
# project marks its comments with symbols; this file cannot, because it is the
# one script that runs on someone else's machine with an unknown code page.
# ---------------------------------------------------------------------------
#
# WHAT IT POINTS AT: THE EXE, THROUGH WINDOWS' CONSOLE HOST, WITH NO WINDOW.
#
# rhoquinn8217, 2026-10-01: "I don't want the terminal to be shown ... I want
# to run it in the background and the tray icon to be the place where it is
# closed."
#
# The listener is a console program, so a shortcut straight to it, or to the
# .bat beside it, opens a terminal window that has to stay open for as long
# as the listener runs. The shortcut therefore runs
#
#     conhost.exe --headless "<this folder>\ctm-usbip.exe" agent 48054 --ui
#
# conhost.exe is the part of Windows that hosts every console program, and
# --headless tells it to host this one with no window. Nothing flashes up,
# and what is left on screen is the DS5-USBIP icon in the tray.
#
# IT USED TO POINT AT start-ctm-usbip.bat, because the .bat checks that
# profiles\ came with the exe and explains itself if it did not. A batch file
# cannot run without a console window, so that check is made HERE instead,
# when the shortcut is created: a missing profiles folder makes the listener
# start normally and then bridge nothing -- a confusing way to fail, and with
# no window there would be nowhere to say so.
#
# The working directory is set on the shortcut itself. The exe reads its
# configs and writes its logs in the folder it is STARTED in, not beside
# itself.

[CmdletBinding()]
param(
    # The folder the shortcut should point INTO. Left empty on purpose and
    # filled in below -- see the note under the param block, which is not a
    # style preference but a bug this hit.
    [string]$FolderPath = '',

    # What the shortcut is called on the desktop. User-visible, so DS5-USBIP.
    [string]$ShortcutName = 'DS5-USBIP'
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# DO NOT WRITE [string]$FolderPath = $PSScriptRoot IN THE PARAM BLOCK ABOVE.
#
# With [CmdletBinding()] present, a parameter DEFAULT that reads $PSScriptRoot
# binds to an EMPTY STRING -- while $PSScriptRoot is perfectly well populated
# two lines later in the body. Advanced-function binding runs before it is
# set. Without [CmdletBinding()] the same default works, which is exactly what
# makes it look fine in a scratch file and fail here.
#
# Measured 2026-09-28: the script died with
#   Cannot bind argument to parameter 'LiteralPath' because it is an empty
#   string
# which names Resolve-Path and says nothing at all about the real cause.
# ---------------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($FolderPath)) { $FolderPath = $PSScriptRoot }

# ABSOLUTE, always. A relative path here would resolve against whatever
# directory the shell happened to be in, and the whole point of a shortcut is
# that it works when nothing else is.
$here = (Resolve-Path -LiteralPath $FolderPath).ProviderPath

$exe      = Join-Path $here 'ctm-usbip.exe'
$profiles = Join-Path $here 'profiles\descriptors'
$conhost  = Join-Path $env:SystemRoot 'System32\conhost.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host ''
    Write-Host 'ERROR: ctm-usbip.exe is not in this folder.' -ForegroundColor Red
    Write-Host ''
    Write-Host "  looked in: $here"
    Write-Host ''
    Write-Host '  Keep this script in the folder it came in. It builds the'
    Write-Host '  shortcut from its own location, so on its own it has nothing'
    Write-Host '  to point at.'
    Write-Host ''
    exit 1
}

if (-not (Test-Path -LiteralPath $profiles)) {
    Write-Host ''
    Write-Host 'ERROR: the profiles folder is not in this folder.' -ForegroundColor Red
    Write-Host ''
    Write-Host "  looked for: $profiles"
    Write-Host ''
    Write-Host '  Extract the whole zip and keep the folder together. Without'
    Write-Host '  profiles the listener starts normally and cannot bridge anything,'
    Write-Host '  and a shortcut that runs it with no window would never say so.'
    Write-Host ''
    exit 1
}

if (-not (Test-Path -LiteralPath $conhost)) {
    Write-Host ''
    Write-Host 'ERROR: this Windows has no conhost.exe where it is expected.' -ForegroundColor Red
    Write-Host ''
    Write-Host "  looked for: $conhost"
    Write-Host ''
    Write-Host '  That is what runs the listener with no window. You can still start'
    Write-Host "  DS5-USBIP by double-clicking start-ctm-usbip.bat in $here"
    Write-Host ''
    exit 1
}

# GetFolderPath, not "$env:USERPROFILE\Desktop". A desktop redirected into
# OneDrive -- which is common, and is the case on the machine this was written
# on -- is not under USERPROFILE at all, and the guessed path would either fail
# or drop the shortcut somewhere nobody looks.
$desktop = [Environment]::GetFolderPath('Desktop')
if ([string]::IsNullOrWhiteSpace($desktop) -or -not (Test-Path -LiteralPath $desktop)) {
    Write-Host ''
    Write-Host 'ERROR: could not find your desktop folder.' -ForegroundColor Red
    Write-Host ''
    Write-Host '  You can still start DS5-USBIP by double-clicking'
    Write-Host "  start-ctm-usbip.bat in $here"
    Write-Host ''
    exit 1
}

$linkPath = Join-Path $desktop ($ShortcutName + '.lnk')
$replacing = Test-Path -LiteralPath $linkPath

$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($linkPath)
$link.TargetPath       = $conhost
# Quoted, because the folder may have spaces in it. The exe's own arguments
# follow it unquoted.
$link.Arguments        = '--headless "' + $exe + '" agent 48054 --ui'
$link.WorkingDirectory = $here
$link.IconLocation     = "$exe,0"
$link.Description      = 'Start DS5-USBIP in the background and open its settings page'
$link.Save()

# Release the COM object rather than waiting for the garbage collector: the
# shortcut file can stay locked otherwise, and the next line reads it back.
[void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell)

if (-not (Test-Path -LiteralPath $linkPath)) {
    Write-Host ''
    Write-Host 'ERROR: the shortcut was not created.' -ForegroundColor Red
    Write-Host "  wanted: $linkPath"
    Write-Host ''
    exit 1
}

Write-Host ''
if ($replacing) {
    Write-Host 'Replaced the DS5-USBIP shortcut on your desktop.' -ForegroundColor Green
} else {
    Write-Host 'Put a DS5-USBIP shortcut on your desktop.' -ForegroundColor Green
}
Write-Host ''
Write-Host "  shortcut   $linkPath"
Write-Host "  runs       $exe agent 48054 --ui"
Write-Host "  starts in  $here"
Write-Host ''
Write-Host '  It runs in the background: no window opens and none stays.'
Write-Host '  Look for the DS5-USBIP icon in the tray, by the clock. Click it for'
Write-Host '  the settings page, and choose Quit there to close it.'
Write-Host ''
Write-Host '  IF YOU MOVE OR RENAME THIS FOLDER, THE SHORTCUT STOPS WORKING.' -ForegroundColor Yellow
Write-Host ''
Write-Host '  A Windows shortcut stores the full path as it is right now, so'
Write-Host '  it cannot follow the folder. Move the folder where you want it'
Write-Host '  FIRST, then run this again -- it will replace the old one.'
Write-Host ''
exit 0
