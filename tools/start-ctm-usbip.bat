@echo off
REM ============================================================================
REM  Starts the CTM-USBIP listener and opens the settings page.
REM
REM  Double-click this. Nothing to configure.
REM
REM  It runs in the BACKGROUND: this window closes by itself and no other one
REM  stays. The DS5-USBIP icon in the tray, by the clock, is where it lives:
REM  click it for the settings page or the on-screen keyboard, and choose Quit
REM  there to close it.
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

REM ============================================================================
REM  ⭐ IT RUNS IN THE BACKGROUND, WITH NO WINDOW (rhoquinn8217, 2026-10-01:
REM  "I don't want the terminal to be shown ... I want to run it in the
REM  background and the tray icon to be the place where it is closed.").
REM
REM  The listener is a console program. Started plainly it brings a terminal
REM  window with it, and that window has to stay open for as long as it runs.
REM  conhost.exe --headless gives it a console with NO window. conhost is the
REM  part of Windows that hosts every console program: not a download, not ours.
REM
REM  ⓘ So THIS window closes as soon as the listener has been started. What is
REM  left is the DS5-USBIP icon in the tray: Open settings, Show keyboard, Quit.
REM  ⛔ Nothing here waits for the listener to end, so there is nothing to
REM  "press any key" on afterwards. Quit on the tray icon is how it is closed.
REM
REM  ⓘ --ui turns the settings API on by itself and opens the page, or brings an
REM  already-open one to the front. Run this while a listener is already up and
REM  that is all it does.
REM
REM  ⚠️ To WATCH it instead, with its log on screen, run it yourself from a
REM  command prompt in this folder:  ctm-usbip.exe agent 48054 --ui
REM ============================================================================
start "" "%SystemRoot%\System32\conhost.exe" --headless "%~dp0ctm-usbip.exe" agent 48054 --ui

exit /b 0
