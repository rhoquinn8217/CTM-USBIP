@echo off
REM ============================================================================
REM  Starts the CTM-USBIP listener and opens the settings page.
REM
REM  Double-click this. Nothing to configure.
REM
REM  ⭐ WHY A .BAT AND NOT A SHORTCUT. A Windows .lnk stores an ABSOLUTE path, so
REM  it breaks the moment the folder is moved or renamed -- and a release is
REM  extracted wherever the user happens to put it. %~dp0 is the directory this
REM  file is sitting in, whatever that turns out to be, so this keeps working.
REM
REM  ⛔ AND WHY NOT JUST RUN THE EXE. Double-clicking ctm-usbip.exe passes no
REM  arguments, so it prints usage and exits -- a console window flashes and
REM  vanishes, which looks like it crashed.
REM ============================================================================

REM  ⚠️ The exe reads its config and writes its logs in the WORKING directory,
REM  not beside itself. Without this cd, configs/ and the logs land wherever the
REM  shell happened to be -- and a setting saved in one place while the agent
REM  reads another looks exactly like a setting that does nothing.
cd /d "%~dp0"

if not exist "ctm-usbip.exe" (
    echo ERROR: ctm-usbip.exe is not in this folder.
    echo.
    echo Keep this file in the folder it came in. The exe needs profiles\ and
    echo maps\ beside it, so moving it on its own will not work.
    echo.
    pause
    exit /b 1
)

if not exist "profiles\descriptors" (
    echo ERROR: the profiles folder is missing.
    echo.
    echo Extract the whole zip and keep the folder together. Without profiles
    echo the listener starts normally and cannot bridge anything, which is a
    echo confusing way to fail.
    echo.
    pause
    exit /b 1
)

echo Starting the listener. The settings page opens in a browser window.
echo.
echo   Settings page   http://127.0.0.1:48055
echo   To stop         close this window, or press Ctrl-C
echo.

REM  ⓘ --ui turns the settings API on by itself and opens the page, or brings an
REM  already-open one to the front.
ctm-usbip.exe agent 48054 --ui

REM  ⚠️ Only pauses on a FAILURE. A normal exit means you closed it deliberately
REM  and there is nothing to read; an error means the window would otherwise
REM  vanish before you could see why.
if errorlevel 1 (
    echo.
    echo The listener exited with an error.
    echo.
    echo  - Is usbip-win2 installed? Nothing can be attached without it.
    echo  - Is another copy already running? Only one can hold the ports.
    echo.
    pause
)

exit /b
