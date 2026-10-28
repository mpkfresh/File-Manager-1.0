#Requires -Version 7.0
<#
Bind the seven media files built on a reviewed Windows host to the review-only
1.0 packaging contract. This writes local records only; it does not approve
media for release, build a package, or upload files.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceCheckout,
    [Parameter(Mandatory)][string]$MediaBuildResult,
    [Parameter(Mandatory)][string]$MediaStage,
    [Parameter(Mandatory)][string]$OutputManifest,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$ExpectedSourceCommit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath($SourceCheckout).TrimEnd('\')
$resultPath = [IO.Path]::GetFullPath($MediaBuildResult)
$stage = [IO.Path]::GetFullPath($MediaStage).TrimEnd('\')
$manifestPath = [IO.Path]::GetFullPath($OutputManifest)
$provenancePath = Join-Path $stage 'manifest\media-provenance.json'
$names = @('avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll',
    'swresample-5.dll', 'swscale-8.dll', 'Qt6Multimedia.dll',
    'multimedia/ffmpegmediaplugin.dll')

function Assert-OrdinaryChain([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Media record path crosses a reparse point.'
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
}
function Assert-Hash([string]$Value, [string]$Name) {
    if ($Value -cnotmatch '^[A-Fa-f0-9]{64}$') {
        throw "Media build result has an invalid $Name hash."
    }
}
function Assert-Pin([string]$Actual, [string]$Expected, [string]$Name) {
    Assert-Hash $Actual $Name
    if ($Actual -ine $Expected) { throw "Media build result differs from the pinned $Name input." }
}

if (-not $manifestPath.StartsWith($workspace + '\',
        [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath (Split-Path -Parent $manifestPath) -PathType Container) -or
        -not (Test-Path -LiteralPath $stage -PathType Container) -or
        -not (Test-Path -LiteralPath $resultPath -PathType Leaf) -or
        (Test-Path -LiteralPath $manifestPath) -or
        (Test-Path -LiteralPath $provenancePath)) {
    throw 'Review media record requires a fresh workspace manifest and existing media inputs.'
}
foreach ($path in @($workspace, $resultPath, $stage, (Split-Path -Parent $manifestPath))) {
    Assert-OrdinaryChain $path
}
$resultItem = Get-Item -LiteralPath $resultPath -Force
if ($resultItem -isnot [IO.FileInfo]) { throw 'Media build result is not an ordinary file.' }
$head = (& git -C $workspace rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or $head.Trim() -ine $ExpectedSourceCommit) {
    throw 'Media record source checkout differs from the approved commit.'
}
$result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
if ($result.schema -cne 'file-manager-media-source-build-v1' -or
        $result.status -cne 'built-unreviewed' -or
        $result.releaseApproved -ne $false -or
        $result.packageBuilt -ne $false) {
    throw 'Media build result is not an unapproved seven-file candidate.'
}
Assert-Pin $result.sourceCompanionSha256 `
    '5123D85407AD0E077A627945E22F220F4D2778DEE1459C610ECB736BA7F66D13' `
    'source companion'
Assert-Pin $result.ffmpegSourceSha256 `
    'E3963A50831C985933E1A625ED566EC4C7ADB5C012C34FA9F84438E1D61BDACC' `
    'FFmpeg source'
Assert-Pin $result.qtMultimediaSourceSha256 `
    '967B5E02EC6B793CDB360622CD6E703132836AF983208D678DAE4B50F109CD9F' `
    'Qt Multimedia source'
Assert-Pin $result.zlibSourceSha256 `
    '9A93B2B7DFDAC77CEBA5A558A580E74667DD6FEDE4585B91EEFB60F03B72DF23' `
    'zlib source'
foreach ($property in @('qtAttributionSha256', 'qtCMakeCacheSha256',
        'qtNinjaGraphSha256')) {
    Assert-Hash ([string]$result.$property) $property
}
$mediaProperties = @($result.mediaFiles.PSObject.Properties.Name | Sort-Object)
if (($mediaProperties -join '|') -cne ((@($names | Sort-Object)) -join '|')) {
    throw 'Media build result must name exactly the seven contracted files.'
}
$actualPaths = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force |
    ForEach-Object { [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/') } |
    Sort-Object)
if (($actualPaths -join '|') -cne ((@($names | Sort-Object)) -join '|')) {
    throw 'Media stage has missing or additional files.'
}
$media = [ordered]@{}
foreach ($name in $names) {
    $path = Join-Path $stage $name.Replace('/', '\')
    Assert-OrdinaryChain $path
    $expected = [string]$result.mediaFiles.PSObject.Properties[$name].Value
    Assert-Hash $expected $name
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $expected) {
        throw "Staged media hash differs: $name"
    }
    $media[$name] = $expected.ToUpperInvariant()
}

$buildRecordHash = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash
$recipeHash = (Get-FileHash -LiteralPath (Join-Path $workspace `
    'tools\Build-ReviewedMediaFromCompanion.ps1') -Algorithm SHA256).Hash
Assert-Pin ([string]$result.buildRecipeSha256) $recipeHash 'build recipe'
$provenance = [ordered]@{
    schema = 'file-manager-media-provenance-v2'
    ffmpegVersion = '7.1.5'
    qtMultimediaVersion = '6.11.2'
    ffmpegSourceArchiveSha256 = $result.ffmpegSourceSha256.ToUpperInvariant()
    ffmpegBuildRecordSha256 = $buildRecordHash
    qtMultimediaSourceArchiveSha256 = $result.qtMultimediaSourceSha256.ToUpperInvariant()
    qtMultimediaBuildRecordSha256 = $buildRecordHash
    qtMultimediaAttributionSha256 = $result.qtAttributionSha256.ToUpperInvariant()
    qtMultimediaCMakeCacheSha256 = $result.qtCMakeCacheSha256.ToUpperInvariant()
    qtMultimediaNinjaGraphSha256 = $result.qtNinjaGraphSha256.ToUpperInvariant()
    qtMultimediaBuildRecipeSha256 = $recipeHash
    zlibSourceArchiveSha256 = $result.zlibSourceSha256.ToUpperInvariant()
    qtShaderToolsArchiveSha256 = 'D434C320F5788CF54C71E68AB28488CEB2F1BA725E63241530155347A4C5081E'
    mediaFiles = $media
}
$manifest = [ordered]@{
    status = 'review-only'
    version = '7.1.5'
    sourceCommit = $ExpectedSourceCommit.ToLowerInvariant()
    sourceArchiveSha256 = $result.ffmpegSourceSha256.ToUpperInvariant()
    buildRecordSha256 = $buildRecordHash
    qtMediaPluginSha256 = $media['multimedia/ffmpegmediaplugin.dll']
    qtMultimediaModuleSha256 = $media['Qt6Multimedia.dll']
    qtMultimediaSourceArchiveSha256 = $result.qtMultimediaSourceSha256.ToUpperInvariant()
    qtMultimediaBuildRecordSha256 = $buildRecordHash
    qtMultimediaAttributionSha256 = $result.qtAttributionSha256.ToUpperInvariant()
    qtMultimediaCMakeCacheSha256 = $result.qtCMakeCacheSha256.ToUpperInvariant()
    qtMultimediaNinjaGraphSha256 = $result.qtNinjaGraphSha256.ToUpperInvariant()
    qtMultimediaBuildRecipeSha256 = $recipeHash
    mediaProvenanceSha256 = ''
    dlls = [ordered]@{
        'avcodec-61.dll' = $media['avcodec-61.dll']
        'avformat-61.dll' = $media['avformat-61.dll']
        'avutil-59.dll' = $media['avutil-59.dll']
        'swresample-5.dll' = $media['swresample-5.dll']
        'swscale-8.dll' = $media['swscale-8.dll']
    }
    pluginProvenanceCleared = $false
    moduleProvenanceCleared = $false
    releaseApproved = $false
}
New-Item -ItemType Directory -Path (Split-Path -Parent $provenancePath) -ErrorAction Stop | Out-Null
$provenance | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath $provenancePath -Encoding utf8NoBOM
$manifest.mediaProvenanceSha256 = (Get-FileHash -LiteralPath $provenancePath -Algorithm SHA256).Hash
$manifest | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
[pscustomobject]@{
    status = 'review-only'
    sourceCommit = $manifest.sourceCommit
    mediaCount = $names.Count
    mediaBuildResultSha256 = $buildRecordHash
    provenanceSha256 = $manifest.mediaProvenanceSha256
    manifestSha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
} | ConvertTo-Json -Depth 3
