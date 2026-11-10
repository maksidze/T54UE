[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$UnrealRoot,
    [Parameter(Mandatory)][string]$Dependencies,
    [string]$Backend = (Join-Path $PSScriptRoot '../T5Bridge'),
    [string]$Generator = 'Visual Studio 18 2026',
    [ValidateSet('Editor', 'Game')][string]$Target = 'Editor',
    [ValidateRange(1, 32)][int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$backendRoot = (Resolve-Path -LiteralPath $Backend).Path
$dependencyRoot = (Resolve-Path -LiteralPath $Dependencies).Path
$engineRoot = (Resolve-Path -LiteralPath $UnrealRoot).Path
$buildUnreal = Join-Path $engineRoot 'Engine/Build/BatchFiles/Build.bat'
if (!(Test-Path -LiteralPath $buildUnreal -PathType Leaf)) {
    throw "UnrealBuildTool entry point not found at $buildUnreal"
}

& (Join-Path $backendRoot 'Build.ps1') -Dependencies $dependencyRoot -Generator $Generator -Targets T5DataBridge -Jobs $Jobs
& "$PSScriptRoot/Plugins/T5Data/Tools/InstallBridge.ps1" `
    -OpenMwSource $backendRoot `
    -OpenMwBuild (Join-Path $backendRoot 'build') `
    -VcpkgBin (Join-Path $dependencyRoot 'installed/x64-windows/bin')

$project = Join-Path $PSScriptRoot 'T54UE.uproject'
$unrealTarget = if ($Target -eq 'Editor') { 'T54UEEditor' } else { 'T54UE' }
& $buildUnreal $unrealTarget Win64 Development "-Project=$project" -WaitMutex -NoHotReloadFromIDE -NoUBA "-MaxParallelActions=$Jobs"
if ($LASTEXITCODE -ne 0) { throw "Unreal build failed with exit code $LASTEXITCODE" }
