# Builds the Release agent + the Inno installer and drops CTM-Bridge-Setup.exe
# into the workspace dist (D:\Work\CTM\dist) — every installer build lands there.
# SAFETY: refuses to package uncommitted agent changes (the quarantined rework
# in agent.inl/bridge.inl) unless -AllowDirty is passed; stash them first:
#   git stash push -- src/app/agent.inl src/backend/bridge.inl
param([switch]$AllowDirty)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $repo

git diff --quiet -- src/app/agent.inl src/backend/bridge.inl
if ($LASTEXITCODE -ne 0 -and -not $AllowDirty) {
    throw "agent.inl / bridge.inl carry uncommitted changes (quarantined rework?). Stash them or pass -AllowDirty."
}

powershell -ExecutionPolicy Bypass -File .\build.ps1 -Configuration Release
if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed" }
# Framework-dependent: ~2MB instead of ~134MB self-contained — requires the
# .NET 10 Desktop Runtime on the target machine (present wherever the SDK is;
# revisit self-contained only if this ever ships to machines without .NET).
dotnet publish apps\CipriansBridge -c Release -r win-x64 --no-self-contained -p:PublishSingleFile=true -p:SelfContained=false -o apps\CipriansBridge\out\publish
if ($LASTEXITCODE -ne 0) { throw "CipriansBridge publish failed" }
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\ctm-usbip.iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }

Copy-Item out\installer\CTM-Bridge-Setup.exe ..\dist\ -Force
Write-Host "dist: $((Resolve-Path ..\dist\CTM-Bridge-Setup.exe).Path)"
