@echo off
REM Opens the DualSense settings panel. Writes ctm-device-config.txt; the
REM agent's watcher applies the change to a running session within a second.
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0device-config-panel.ps1" %*
