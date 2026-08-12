@echo off
REM Opens the DualSense EDGE settings panel. Writes the [ds5_edge] section of
REM ctm-device-config.txt; the agent's watcher applies the change to a running
REM session within a second.
REM
REM The Edge reads ONLY its own section -- it does not inherit from [ds5] --
REM so the plain panel cannot configure it. Use that one for a standard
REM DualSense and this one for an Edge.
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0device-config-panel-edge.ps1" %*
