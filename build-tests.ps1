# Builds and runs the unit tests. Separate from build.ps1 on purpose:
#   - build.ps1 is a file upstream also edits, so leaving it untouched keeps
#     our merge surface at zero for this feature.
#   - tests are a deliberate pre-push step, not a tax on every ordinary build.
#
# Toolchain discovery below is MIRRORED from build.ps1 @ 19f22a9 (Find-
# VisualStudio / Import-VsDevEnvironment / Find-MSBuild). Copied rather than
# shared because build.ps1 is a script, not a module -- dot-sourcing it would
# execute a full product build. If build.ps1's discovery changes upstream,
# re-check these three functions.
[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot

function Find-VisualStudio {
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vsWhere)) { return $null }
    & $vsWhere -latest -products * -version '[17.0,19.0)' -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
}

function Import-VsDevEnvironment([string]$VsInstall, [string]$Arch) {
    $vcvarsall = Join-Path $VsInstall 'VC\Auxiliary\Build\vcvarsall.bat'
    if (-not (Test-Path $vcvarsall)) { throw "vcvarsall.bat not found: $vcvarsall" }
    $env:Path = $env:Path -replace '"', ''
    $envLines = cmd.exe /c "`"$vcvarsall`" $Arch > nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw 'vcvarsall failed' }
    foreach ($line in $envLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:\$($Matches[1])" -Value $Matches[2]
        }
    }
}

function Find-MSBuild([string]$VsInstall) {
    $candidates = @(
        (Join-Path $VsInstall 'MSBuild\Current\Bin\amd64\MSBuild.exe'),
        (Join-Path $VsInstall 'MSBuild\Current\Bin\MSBuild.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$vs = Find-VisualStudio
if (-not $vs) { throw 'Visual Studio Native Desktop workload not found.' }
$vs = $vs.Trim()
Import-VsDevEnvironment -VsInstall $vs -Arch $Platform
$msbuild = Find-MSBuild -VsInstall $vs
if (-not $msbuild) { throw 'MSBuild.exe not found.' }

$project = Join-Path $Root 'app\ctm-usbip-tests.vcxproj'
& $msbuild $project /m /p:Configuration=$Configuration /p:Platform=$Platform
if ($LASTEXITCODE -ne 0) { throw 'ctm-usbip-tests build failed.' }

$out = Join-Path $Root "out\$Platform\$Configuration"
$exe = Join-Path $out 'ctm-usbip-tests.exe'
$map = Join-Path $Root 'maps\ds5_usb_over_ds5_usb.map'
$btMap = Join-Path $Root 'maps\ds5_usb_over_ds5_bt.map'

Write-Host ''
Write-Host '--- running tests ---'
# Run from the output directory so the control test's temp map lands in
# already-ignored build output rather than the repo root.
Push-Location $out
try {
    & $exe $map $btMap
    $testExit = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($testExit -ne 0) {
    Write-Host 'TESTS FAILED' -ForegroundColor Red
    exit $testExit
}
Write-Host 'tests passed' -ForegroundColor Green
