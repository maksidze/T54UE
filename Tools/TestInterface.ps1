[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$UnrealRoot,
    [string]$Data = (Join-Path $PSScriptRoot '../Data'),
    [ValidatePattern('^[A-Za-z_-]+$')][string]$Language,
    [string]$Output
)

# Developer-only visual check. Container, dialogue and trade use sample data in the production widgets.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$project = Join-Path $root 'T54UE.uproject'
$data = (Resolve-Path -LiteralPath $Data).Path
$editor = (Resolve-Path -LiteralPath (Join-Path $UnrealRoot 'Engine/Binaries/Win64/UnrealEditor.exe')).Path
if (!$Output) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $Output = Join-Path $root "Saved/Screenshots/T5UIReview/$stamp"
}
$outputDir = (New-Item -ItemType Directory -Force $Output).FullName
$log = Join-Path $outputDir 'review.log'
$argsForEditor = @(
    ('"{0}"' -f $project)
    '-game', '-unattended', '-nosplash', '-nosound', '-NoScreenMessages', '-RenderOffscreen'
    '-windowed', '-ForceRes', '-ResX=1600', '-ResY=900'
    '-T5Map=RiverwoodRiverwoodTrader', '-T5ReadOnly', '-T5PapyrusContinueOnMissingNative'
    ('-T5UIReview="{0}"' -f $outputDir)
    ('-T5PapyrusLog="{0}"' -f (Join-Path $outputDir 'papyrus.log'))
    ('-abslog="{0}"' -f $log)
    '-ExecCmds="Automation RunTests T5Data.UI.Review"'
    '-TestExit="Automation Test Queue Empty"'
)
if ($PSBoundParameters.ContainsKey('Data')) {
    $argsForEditor += ('-T5Data="{0}"' -f $data)
}
if ($Language) {
    $argsForEditor += "-T5Language=$Language"
}
$process = Start-Process -FilePath $editor -ArgumentList $argsForEditor -WorkingDirectory $root -WindowStyle Hidden -PassThru
if (!$process.WaitForExit(240000)) {
    $process.Kill()
    throw "UI review timed out. See $log"
}
if ($process.ExitCode -ne 0 -or !(Select-String -LiteralPath $log -Quiet -Pattern 'Result=\{Success\}.*T5Data.UI.Review')) {
    throw "UI review failed. See $log"
}

$bsa = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead((Join-Path $data 'Skyrim - Interface.bsa')))
try {
    $bsa.BaseStream.Position = 20
    $fileCount = $bsa.ReadUInt32()
} finally {
    $bsa.Dispose()
}
$catalogPath = Join-Path $root 'Saved/T5UICache/catalog.json'
$catalog = Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json
if ($catalog.entries.Count -ne $fileCount -or
    @($catalog.entries.path | Sort-Object -Unique).Count -ne $fileCount) {
    throw "The catalog does not match the archive's $fileCount file entries."
}
foreach ($asset in $catalog.art.PSObject.Properties) {
    $bitmap = $asset.Value
    if ($bitmap.source -notin $catalog.entries.path -or !$bitmap.symbol) {
        throw "Missing source movie or symbol for $($asset.Name)."
    }
    $bytes = [Convert]::FromBase64String($bitmap.bgra)
    if ($bytes.Length -ne $bitmap.width * $bitmap.height * 4) {
        throw "Invalid bitmap size for $($asset.Name)."
    }
    $hasInk = $false
    for ($pixel = 3; $pixel -lt $bytes.Length; $pixel += 4) {
        if ($bytes[$pixel] -gt 0) { $hasInk = $true; break }
    }
    if (!$hasInk) { throw "Empty bitmap for $($asset.Name)." }
}
$fontFaces = @($catalog.fonts.PSObject.Properties.Name)
$expectedConfig = "interface/fontconfig_$($catalog.language).txt"
if ($expectedConfig -notin $catalog.entries.path) { $expectedConfig = 'interface/fontconfig.txt' }
if ($catalog.fontConfig -ne $expectedConfig) {
    throw "Expected $expectedConfig, got $($catalog.fontConfig)."
}
foreach ($role in @('Regular', 'Medium', 'Bold', 'Dialogue')) {
    if ($catalog.fontRoles.$role -notin $fontFaces) {
        throw "The cache does not contain an original interface font for $role."
    }
}
if (@(Select-String -LiteralPath $log -Pattern 'T5_ITEM_PREVIEW item=').Count -lt 2) {
    throw "The inventory and container previews must render an imported item model. See $log"
}
Write-Output "Verified $fileCount archive entries, $($catalog.art.PSObject.Properties.Name.Count) bitmaps, $($fontFaces.Count) fonts from $($catalog.fontConfig) and both item previews."
Write-Output $outputDir
