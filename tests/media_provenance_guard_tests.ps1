#Requires -Version 7.0
# Synthetic contract fixtures only. These files are never release inputs.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
. (Join-Path $workspace 'packaging\ReviewedFfmpeg.ps1')

$fixtureId = [Guid]::NewGuid().ToString('N')
$manifestRoot = Join-Path $workspace "Testing\synthetic-media-guard-$fixtureId"
$runtimeRoot = Join-Path ([IO.Path]::GetTempPath()) "file-manager-synthetic-media-guard-$fixtureId"
$manifestRelative = "Testing\synthetic-media-guard-$fixtureId\reviewed.json"
$manifestPath = Join-Path $manifestRoot 'reviewed.json'
$provenancePath = Join-Path $runtimeRoot 'manifest\media-provenance.json'

function Get-FixtureHash([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Write-FixtureJson([string]$Path, $Value) {
    $Value | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}

function Invoke-FixtureGuard([string]$ManifestHash) {
    Get-ReviewedFfmpegInputs -Workspace $workspace `
        -ManifestPath $manifestRelative -ManifestSha256 $ManifestHash `
        -RuntimeDirectory $runtimeRoot -RequireReleaseApproval
}

function Assert-FixtureRefused([string]$Label, [string]$ManifestHash, [string]$ExpectedMessage) {
    try {
        [void](Invoke-FixtureGuard $ManifestHash)
    } catch {
        if ($_.Exception.Message -cnotlike "*$ExpectedMessage*") {
            throw
        }
        return [pscustomobject]@{ case = $Label; refused = $true }
    }
    throw "Synthetic release guard accepted invalid fixture: $Label"
}

try {
    foreach ($path in @($manifestRoot, $runtimeRoot)) {
        if (Test-Path -LiteralPath $path) {
            throw "Synthetic fixture already exists: $path"
        }
    }
    [IO.Directory]::CreateDirectory($manifestRoot) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $runtimeRoot 'multimedia')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $runtimeRoot 'manifest')) | Out-Null

    $mediaNames = @('avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll',
        'swresample-5.dll', 'swscale-8.dll', 'Qt6Multimedia.dll',
        'multimedia/ffmpegmediaplugin.dll')
    $mediaFiles = [ordered]@{}
    foreach ($name in $mediaNames) {
        $path = Join-Path $runtimeRoot $name.Replace('/', '\')
        [IO.File]::WriteAllText($path, "synthetic fixture: $name`n")
        $mediaFiles[$name] = Get-FixtureHash $path
    }
    $provenance = [ordered]@{
        schema = 'file-manager-media-provenance-v2'
        ffmpegVersion = '7.1.5'
        qtMultimediaVersion = '6.11.2'
        ffmpegSourceArchiveSha256 = 'E3963A50831C985933E1A625ED566EC4C7ADB5C012C34FA9F84438E1D61BDACC'
        ffmpegBuildRecordSha256 = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
        qtMultimediaSourceArchiveSha256 = 'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB'
        qtMultimediaBuildRecordSha256 = 'CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC'
        qtMultimediaAttributionSha256 = '24D5FFF2E100FB7C2531771D594A008992F85406387EFC9BA6889D5C51EFC343'
        qtMultimediaCMakeCacheSha256 = 'EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE'
        qtMultimediaNinjaGraphSha256 = 'FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF'
        qtMultimediaBuildRecipeSha256 = '1111111111111111111111111111111111111111111111111111111111111111'
        zlibSourceArchiveSha256 = '9A93B2B7DFDAC77CEBA5A558A580E74667DD6FEDE4585B91EEFB60F03B72DF23'
        qtShaderToolsArchiveSha256 = 'D434C320F5788CF54C71E68AB28488CEB2F1BA725E63241530155347A4C5081E'
        mediaFiles = $mediaFiles
    }
    Write-FixtureJson $provenancePath $provenance
    $manifest = [ordered]@{
        version = '7.1.5'
        status = 'release-approved'
        releaseApproved = $true
        pluginProvenanceCleared = $true
        moduleProvenanceCleared = $true
        sourceArchiveSha256 = $provenance.ffmpegSourceArchiveSha256
        buildRecordSha256 = $provenance.ffmpegBuildRecordSha256
        qtMultimediaSourceArchiveSha256 = $provenance.qtMultimediaSourceArchiveSha256
        qtMultimediaBuildRecordSha256 = $provenance.qtMultimediaBuildRecordSha256
        qtMultimediaAttributionSha256 = $provenance.qtMultimediaAttributionSha256
        qtMultimediaCMakeCacheSha256 = $provenance.qtMultimediaCMakeCacheSha256
        qtMultimediaNinjaGraphSha256 = $provenance.qtMultimediaNinjaGraphSha256
        qtMultimediaBuildRecipeSha256 = $provenance.qtMultimediaBuildRecipeSha256
        mediaProvenanceSha256 = (Get-FixtureHash $provenancePath)
        dlls = [ordered]@{
            'avcodec-61.dll' = $mediaFiles['avcodec-61.dll']
            'avformat-61.dll' = $mediaFiles['avformat-61.dll']
            'avutil-59.dll' = $mediaFiles['avutil-59.dll']
            'swresample-5.dll' = $mediaFiles['swresample-5.dll']
            'swscale-8.dll' = $mediaFiles['swscale-8.dll']
        }
        qtMediaPluginSha256 = $mediaFiles['multimedia/ffmpegmediaplugin.dll']
        qtMultimediaModuleSha256 = $mediaFiles['Qt6Multimedia.dll']
    }
    Write-FixtureJson $manifestPath $manifest
    try {
        $accepted = Invoke-FixtureGuard (Get-FixtureHash $manifestPath)
    } catch {
        $loaded = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        $checks = [ordered]@{
            version = $loaded.version -ceq '7.1.5'
            sourceArchive = $loaded.sourceArchiveSha256 -ieq $provenance.ffmpegSourceArchiveSha256
            status = $loaded.status -ceq 'release-approved'
            releaseApproved = (($loaded.releaseApproved -is [bool]) -and $loaded.releaseApproved)
            pluginCleared = (($loaded.pluginProvenanceCleared -is [bool]) -and $loaded.pluginProvenanceCleared)
            moduleCleared = (($loaded.moduleProvenanceCleared -is [bool]) -and $loaded.moduleProvenanceCleared)
        }
        foreach ($propertyName in @('qtMultimediaModuleSha256',
                'qtMultimediaSourceArchiveSha256', 'qtMultimediaBuildRecordSha256',
                'qtMultimediaAttributionSha256', 'qtMultimediaCMakeCacheSha256',
                'qtMultimediaNinjaGraphSha256', 'qtMultimediaBuildRecipeSha256',
                'mediaProvenanceSha256')) {
            $checks[$propertyName] = ([string]$loaded.$propertyName) -cmatch '^[A-Fa-f0-9]{64}$'
        }
        $failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value } |
            ForEach-Object { $_.Key })
        throw "Synthetic fixture baseline rejected; failed fields: $($failed -join ', ')."
    }
    if ($accepted.Sources.Count -ne 8) {
        throw 'Synthetic valid fixture did not bind seven media files and one provenance record.'
    }

    $results = [Collections.Generic.List[object]]::new()
    $manifest.releaseApproved = $false
    Write-FixtureJson $manifestPath $manifest
    $manifestHash = Get-FixtureHash $manifestPath
    $results.Add((Assert-FixtureRefused 'approval-disabled' $manifestHash 'requires approved FFmpeg'))
    $manifest.releaseApproved = $true

    $provenance.qtMultimediaNinjaGraphSha256 =
        '2222222222222222222222222222222222222222222222222222222222222222'
    Write-FixtureJson $provenancePath $provenance
    $manifest.mediaProvenanceSha256 = Get-FixtureHash $provenancePath
    Write-FixtureJson $manifestPath $manifest
    $manifestHash = Get-FixtureHash $manifestPath
    $results.Add((Assert-FixtureRefused 'build-graph-mismatch' $manifestHash 'source/build contract differs'))
    $provenance.qtMultimediaNinjaGraphSha256 = $manifest.qtMultimediaNinjaGraphSha256
    Write-FixtureJson $provenancePath $provenance
    $manifest.mediaProvenanceSha256 = Get-FixtureHash $provenancePath
    Write-FixtureJson $manifestPath $manifest

    [IO.File]::AppendAllText((Join-Path $runtimeRoot 'avutil-59.dll'), 'changed')
    $manifestHash = Get-FixtureHash $manifestPath
    $results.Add((Assert-FixtureRefused 'media-byte-mismatch' $manifestHash 'Reviewed FFmpeg runtime differs'))

    [pscustomobject]@{
        status = 'passed-synthetic-guard-only'
        acceptedFixture = $true
        refusalCases = @($results)
        actualMediaReleaseApproved = $false
        packageBuilt = $false
    } | ConvertTo-Json -Depth 5
} finally {
    $expectedManifestPrefix = [IO.Path]::GetFullPath((Join-Path $workspace 'Testing')) + '\'
    $expectedRuntimePrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $manifestItem = Get-Item -LiteralPath $manifestRoot -Force -ErrorAction SilentlyContinue
    if ($null -ne $manifestItem -and
            $manifestItem -is [IO.DirectoryInfo] -and
            ($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -and
            $manifestItem.FullName.StartsWith($expectedManifestPrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $manifestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    $runtimeItem = Get-Item -LiteralPath $runtimeRoot -Force -ErrorAction SilentlyContinue
    if ($null -ne $runtimeItem -and
            $runtimeItem -is [IO.DirectoryInfo] -and
            ($runtimeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -and
            $runtimeItem.FullName.StartsWith($expectedRuntimePrefix,
                [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $runtimeRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
