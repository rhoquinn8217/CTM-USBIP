# install-ctm-vdd.ps1 — install the vendored, signed VirtualDisplayDriver (VDD)
# configured to mirror the target LG HDR 4K TV (user_edid.bin) with HDR enabled.
# Self-elevates. The driver is signed by SignPath Foundation, so no test-signing
# is required.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Write-Host 'Requesting elevation...'
    Start-Process powershell.exe -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    return
}

# 1. Place the LG EDID + settings where the driver reads them.
$cfg = 'C:\VirtualDisplayDriver'
New-Item -ItemType Directory -Force -Path $cfg | Out-Null
Copy-Item (Join-Path $here 'vdd_settings.xml') $cfg -Force
Copy-Item (Join-Path $here 'user_edid.bin')   $cfg -Force
Write-Host "Placed vdd_settings.xml + user_edid.bin in $cfg"

# 2. Install the signed driver as a root-enumerated device.
$inf = Join-Path $here 'x64\MttVDD.inf'
& (Join-Path $here 'devcon.exe') install $inf 'Root\MttVDD'
if ($LASTEXITCODE -ne 0) { Write-Warning "devcon returned $LASTEXITCODE" }

Write-Host ''
Write-Host 'Done. Expect a "VirtualDisplayDriver Device" in Device Manager and an'
Write-Host '"LG HDR 4K" virtual monitor in Settings > Display (toggle HDR there).'
Write-Host 'If nothing appears, run with logging=true in vdd_settings.xml and re-check.'
Read-Host 'Press Enter to close'
