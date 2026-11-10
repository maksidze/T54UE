param(
    [Parameter(Mandatory)][string]$Mods,
    [string]$SevenZip = 'C:\Program Files\7-Zip\7z.exe'
)
$ErrorActionPreference = 'Stop'
$plugin = Split-Path -Parent $PSScriptRoot
$target = Join-Path $plugin 'Resources/RaceMenu'
New-Item -ItemType Directory -Path $target -Force | Out-Null
$packages = @(
    @{ Pattern = 'RaceMenu Anniversary*.7z'; Files = @('RaceMenu.bsa','ModderResource/IPluginInterface.h','SKSE/Plugins/skee64.ini') },
    @{ Pattern = 'Modders Package*.7z'; Files = @('source/scripts/*') },
    @{ Pattern = 'Modified Overlay Meshes*.7z'; Files = @('meshes/*') }
)
$sources = @()
foreach ($package in $packages) {
    $matches = @(Get-ChildItem -LiteralPath $Mods -Filter $package.Pattern -File)
    if ($matches.Count -ne 1) { throw "Expected exactly one archive matching $($package.Pattern)" }
    $archive = $matches[0]
    & $SevenZip x $archive.FullName "-o$target" '-y' @($package.Files) | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Archive extraction failed: $($archive.Name)" }
    $sources += @{ file = $archive.Name; sha256 = (Get-FileHash -LiteralPath $archive.FullName).Hash }
}
# Category IDs are RaceMenu's published Papyrus contract, not guessed UE enum values.
$script = Get-Content -LiteralPath (Join-Path $target 'source/scripts/RaceMenuBase.psc') -Raw
$categories = @([regex]::Matches($script, '(?im)^int Property CATEGORY_(\w+)\s*=\s*(\d+)') | ForEach-Object {
    @{ name = $_.Groups[1].Value; id = [int]$_.Groups[2].Value }
})
if ($categories.Count -ne 9) { throw 'Unexpected RaceMenuBase category contract' }
@{ version = 1; categories = $categories; sources = $sources; sourceDirectory = $Mods
    runtime = 'T54UE Slate + TES5 records/TRI'; skseBinaryLoaded = $false
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $target 'manifest.json') -Encoding utf8
Write-Output "Installed RaceMenu resources and $($categories.Count) category IDs into $target"
