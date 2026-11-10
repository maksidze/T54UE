param(
    [Parameter(Mandatory)][string]$OpenMwSource,
    [Parameter(Mandatory)][string]$OpenMwBuild,
    [Parameter(Mandatory)][string]$VcpkgBin,
    [string[]]$PackagedRoots = @()
)

$ErrorActionPreference = 'Stop'
$plugin = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $plugin 'Binaries/ThirdParty/Win64'
New-Item -ItemType Directory -Force -Path $runtime, "$runtime/Resources", "$runtime/osgPlugins-3.6.5" | Out-Null
Copy-Item -LiteralPath (Join-Path $OpenMwBuild 'Release/T5DataBridge.dll') -Destination $runtime
Copy-Item -LiteralPath (Join-Path $OpenMwSource 'files/settings-default.cfg') -Destination "$runtime/Resources"
foreach ($name in 'OpenThreads','osg','osgDB','osgGA','osgParticle','osgText','osgUtil','osgViewer',
    'lz4','zlib1','libpng16','jpeg62','freetype','bz2','brotlidec','brotlicommon') {
    Copy-Item -LiteralPath (Join-Path $VcpkgBin "$name.dll") -Destination $runtime
}
foreach ($name in 'avcodec','avformat','avutil','swresample') {
    $library = @(Get-ChildItem -LiteralPath (Join-Path $OpenMwBuild 'Release') -Filter "$name-*.dll" -File)
    if ($library.Count -ne 1) { throw "Expected one $name runtime DLL beside T5DataBridge.dll" }
    Copy-Item -LiteralPath $library[0].FullName -Destination $runtime
}
$myGui = Join-Path $VcpkgBin 'MyGUIEngine.dll'
if (!(Test-Path -LiteralPath $myGui)) { $myGui = Join-Path $VcpkgBin 'Release/MyGUIEngine.dll' }
Copy-Item -LiteralPath $myGui -Destination $runtime
foreach ($format in 'dds','jpeg','png','tga') {
    Copy-Item -LiteralPath (Join-Path $VcpkgBin "osgPlugins-3.6.5/osgdb_$format.dll") -Destination "$runtime/osgPlugins-3.6.5"
}

# Deploy to every runtime that will actually be launched, not just the UE project.
foreach ($packageRoot in $PackagedRoots) {
    $target = Join-Path $packageRoot 'T54UE/Plugins/T5Data/Binaries/ThirdParty/Win64'
    if (!(Test-Path -LiteralPath $target -PathType Container)) { throw "Missing packaged native runtime: $target" }
    Copy-Item -LiteralPath (Join-Path $runtime 'T5DataBridge.dll') -Destination $target -Force
    if ((Get-FileHash -LiteralPath (Join-Path $target 'T5DataBridge.dll')).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $runtime 'T5DataBridge.dll')).Hash) { throw "Bridge deployment mismatch: $target" }
}
