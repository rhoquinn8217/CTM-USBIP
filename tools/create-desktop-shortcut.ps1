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
# WHAT IT POINTS AT, AND WHY NOT THE EXE.
#
# The shortcut targets start-ctm-usbip.bat rather than ctm-usbip.exe. The .bat
# sets the working directory, checks that profiles\ and maps\ came with the
# exe, and explains itself if they did not. A shortcut straight to the exe
# would skip all of that, and a missing profiles folder makes the listener
# start normally and then bridge nothing -- a confusing way to fail.
#
# The icon still comes from the exe, so it does not look like a batch file.

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
$launcher = Join-Path $here 'start-ctm-usbip.bat'

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

if (-not (Test-Path -LiteralPath $launcher)) {
    Write-Host ''
    Write-Host 'ERROR: start-ctm-usbip.bat is not in this folder.' -ForegroundColor Red
    Write-Host ''
    Write-Host "  looked in: $here"
    Write-Host ''
    Write-Host '  That file is what the shortcut runs. Extract the whole zip.'
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
$link.TargetPath       = $launcher
$link.WorkingDirectory = $here
$link.IconLocation     = "$exe,0"
$link.Description      = 'Start the DS5-USBIP listener and open its settings page'
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
Write-Host "  runs       $launcher"
Write-Host "  starts in  $here"
Write-Host ''
Write-Host '  IF YOU MOVE OR RENAME THIS FOLDER, THE SHORTCUT STOPS WORKING.' -ForegroundColor Yellow
Write-Host ''
Write-Host '  A Windows shortcut stores the full path as it is right now, so'
Write-Host '  it cannot follow the folder. Move the folder where you want it'
Write-Host '  FIRST, then run this again -- it will replace the old one.'
Write-Host ''
exit 0
