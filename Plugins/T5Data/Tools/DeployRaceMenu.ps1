param([Parameter(Mandatory)][string[]]$PackagedRoots)
$ErrorActionPreference = 'Stop'
$plugin = Split-Path -Parent $PSScriptRoot
$project = Split-Path -Parent (Split-Path -Parent $plugin)
$executable = Join-Path $project 'Binaries/Win64/T54UE.exe'
$bridge = Join-Path $plugin 'Binaries/ThirdParty/Win64/T5DataBridge.dll'
$resources = Join-Path $plugin 'Resources/RaceMenu'
foreach ($root in $PackagedRoots) {
    $runtime = Join-Path $root 'T54UE'
    if (!(Test-Path -LiteralPath (Join-Path $runtime 'Binaries/Win64/T54UE.exe'))) { throw "Missing packaged runtime: $root" }
    Copy-Item -LiteralPath $executable -Destination (Join-Path $runtime 'Binaries/Win64/T54UE.exe') -Force
    Copy-Item -LiteralPath $bridge -Destination (Join-Path $runtime 'Plugins/T5Data/Binaries/ThirdParty/Win64/T5DataBridge.dll') -Force
    $destination = Join-Path $runtime 'Plugins/T5Data/Resources'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $resources -Destination $destination -Recurse -Force
    if ((Get-FileHash -LiteralPath $executable).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $runtime 'Binaries/Win64/T54UE.exe')).Hash) { throw 'Executable deployment mismatch' }
    if ((Get-FileHash -LiteralPath $bridge).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $runtime 'Plugins/T5Data/Binaries/ThirdParty/Win64/T5DataBridge.dll')).Hash) { throw 'Bridge deployment mismatch' }
    Write-Output "Deployed RaceMenu code and resources to $root"
}
