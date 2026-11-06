#Requires -Version 7.0
<#
Stages the seven exact, previously approved V53 media files for a hosted
File Manager build. The archive is a pinned binary input, not a new build.
No network request, package, upload, signing, or release occurs here.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArchivePath,
    [Parameter(Mandatory)][string]$ManifestPath,
    [Parameter(Mandatory)][string]$OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$archiveSha256 = '2D0D037BD8B6DB9E9D24A8EFA8FDDD0A7DED197E2041201E933044F1DB0DA588'
$archiveBytes = 9128997L
$manifestSha256 = 'AE68D738D51E7B5D07A68BDA0B07E85F33615B1192847BB68FE04CBC7F49E61C'
$sourceArtifactSha256 = '1C0F6F177A1D8B26B7DFA3D03FFF84F4EC8325FE125D242032C2DF1976009511'
$sourceCommit = '42450e674dd5b25fc3d92b499310a8dcf44b05c8'
$expectedSizes = [ordered]@{
    'avcodec-61.dll' = 14061056L
    'avformat-61.dll' = 2640384L
    'avutil-59.dll' = 1162752L
    'swresample-5.dll' = 231936L
    'swscale-8.dll' = 745984L
    'Qt6Multimedia.dll' = 1287168L
    'multimedia/ffmpegmediaplugin.dll' = 689152L
}

$archive = Get-Item -LiteralPath $ArchivePath -Force -ErrorAction Stop
$manifestFile = Get-Item -LiteralPath $ManifestPath -Force -ErrorAction Stop
foreach ($item in @($archive, $manifestFile)) {
    if ($item -isnot [IO.FileInfo] -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The reviewed media input must be an ordinary file.'
    }
}
if ($archive.Length -ne $archiveBytes -or
        (Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash -ine $archiveSha256 -or
        (Get-FileHash -LiteralPath $manifestFile.FullName -Algorithm SHA256).Hash -ine $manifestSha256) {
    throw 'The pinned V53 archive or approved manifest differs.'
}
$approved = Get-Content -Raw -LiteralPath $manifestFile.FullName | ConvertFrom-Json
if ($approved.status -cne 'release-approved' -or $approved.releaseApproved -ne $true -or
        $approved.sourceCommit -cne $sourceCommit) {
    throw 'The approved V53 manifest has an unexpected status or source.'
}
$expected = [ordered]@{}
foreach ($name in @('avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll',
        'swresample-5.dll', 'swscale-8.dll')) {
    $expected[$name] = [string]$approved.dlls.PSObject.Properties[$name].Value
}
$expected['Qt6Multimedia.dll'] = [string]$approved.qtMultimediaModuleSha256
$expected['multimedia/ffmpegmediaplugin.dll'] = [string]$approved.qtMediaPluginSha256
if ((@($expected.Keys | Sort-Object) -join '|') -cne
        (@($expectedSizes.Keys | Sort-Object) -join '|') -or
        @($expected.Values | Where-Object { $_ -cnotmatch '^[A-F0-9]{64}$' }).Count -ne 0) {
    throw 'The approved V53 manifest does not name exactly seven expected files.'
}

$output = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
if (Test-Path -LiteralPath $output) { throw 'The media output root must be fresh.' }
$parent = Get-Item -LiteralPath (Split-Path -Parent $output) -Force -ErrorAction Stop
if ($parent -isnot [IO.DirectoryInfo]) { throw 'The media output parent is not a directory.' }

Add-Type -AssemblyName System.IO.Compression
$zip = [IO.Compression.ZipFile]::OpenRead($archive.FullName)
try {
    $entries = @($zip.Entries)
    if ($entries.Count -ne 7 -or
            (@($entries | ForEach-Object FullName | Sort-Object) -join '|') -cne
            (@($expected.Keys | Sort-Object) -join '|')) {
        throw 'The pinned V53 archive contains unexpected entries.'
    }
    foreach ($entry in $entries) {
        if ($entry.FullName -notin @($expected.Keys) -or
                $entry.Length -ne $expectedSizes[$entry.FullName] -or
                $entry.FullName.StartsWith('/') -or
                $entry.FullName.Contains('..') -or
                $entry.FullName.Contains('\')) {
            throw 'A pinned V53 archive entry has an unsafe path or size.'
        }
    }
    New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
    $stage = Join-Path $output 'seven-media-stage'
    $reports = Join-Path $output 'reports'
    New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory -Path $reports -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage 'multimedia') -ErrorAction Stop | Out-Null
    foreach ($entry in $entries) {
        $destination = Join-Path $stage $entry.FullName.Replace('/', '\')
        $sourceStream = $entry.Open()
        $targetStream = [IO.File]::Create($destination)
        try { $sourceStream.CopyTo($targetStream) }
        finally { $targetStream.Dispose(); $sourceStream.Dispose() }
        $item = Get-Item -LiteralPath $destination -Force -ErrorAction Stop
        if ($item.Length -ne $expectedSizes[$entry.FullName] -or
                (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ine
                    $expected[$entry.FullName]) {
            throw "A staged V53 media file differs: $($entry.FullName)"
        }
    }
} finally {
    $zip.Dispose()
}

$result = [ordered]@{
    schema = 'file-manager-approved-retained-media-input-v1'
    status = 'approved-retained-input'
    packagingInputApproved = $true
    finalReleaseApproved = $false
    packageBuilt = $false
    uploaded = $false
    sourceRunId = 41370308365
    sourceCommit = $sourceCommit
    sourceArtifactSha256 = $sourceArtifactSha256
    inputArchiveSha256 = $archiveSha256
    approvedManifestSha256 = $manifestSha256
    mediaProvenanceSha256 = [string]$approved.mediaProvenanceSha256
    mediaFiles = $expected
}
$reportPath = Join-Path $reports 'media-input-result.json'
$result | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath $reportPath -Encoding utf8NoBOM
Write-Output ($result | ConvertTo-Json -Depth 5 -Compress)
