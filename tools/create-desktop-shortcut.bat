@echo off
REM ============================================================================
REM  Puts a DS5-USBIP shortcut on your desktop.
REM
REM  Double-click this. Nothing to configure.
REM
REM  WHY A .BAT WRAPPER AROUND A .PS1. Double-clicking a .ps1 does not run it --
REM  Windows opens it in Notepad. There is no way to change that from inside the
REM  file, so the thing the user clicks has to be a .bat.
REM
REM  AND WHY -ExecutionPolicy Bypass. The default policy on a home machine is
REM  Restricted, which refuses to run any script at all. Bypass applies to THIS
REM  ONE invocation and changes nothing on the machine -- it is not the same as
REM  Set-ExecutionPolicy, which would.
REM ============================================================================

cd /d "%~dp0"

if not exist "create-desktop-shortcut.ps1" (
    echo ERROR: create-desktop-shortcut.ps1 is not next to this file.
    echo.
    echo Extract the whole zip and keep the folder together.
    echo.
    pause
    exit /b 1
)

REM  -NoProfile so a user's own profile script cannot change what this does.
REM
REM  DO NOT ADD -FolderPath "%~dp0" HERE. It looks harmless and is not:
REM  %~dp0 ends with a \, so the quoted argument ends \" -- an ESCAPED QUOTE.
REM  PowerShell then swallows the closing quote, the path runs on into the
REM  next argument, and Resolve-Path fails with ItemExistsArgumentError.
REM  Measured 2026-09-28, on the first run of this file.
REM
REM  It is not needed anyway: the .ps1 defaults -FolderPath to $PSScriptRoot,
REM  which is its own directory WITHOUT the trailing backslash.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0create-desktop-shortcut.ps1"

REM  Always pauses. This prints a result worth reading -- where the shortcut
REM  went, and the warning about moving the folder -- and a double-clicked
REM  window would otherwise close before it could be read.
echo.
pause

exit /b
