# uninstall-ctm-vdd.ps1 — remove the VDD virtual display device and its config.
# Self-elevates. Leaves the driver package in the store (harmless); use
# pnputil /enum-drivers + /delete-driver oemNN.inf to purge it fully.
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    return
}

& (Join-Path $here 'devcon.exe') remove 'Root\MttVDD'
Remove-Item 'C:\VirtualDisplayDriver' -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'Removed the VDD virtual display device and C:\VirtualDisplayDriver.'
Read-Host 'Press Enter to close'
