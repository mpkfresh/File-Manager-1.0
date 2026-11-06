#Requires -Version 7.0
<#
Create and verify a final-identity unsigned 1.0 MSI and portable ZIP as
signing inputs. This script does not upload, sign, install or publish them.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceCheckout,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{40}$')][string]$ExpectedSourceCommit,
    [Parameter(Mandatory)][string]$ApprovedMediaManifest,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExpectedApprovedMediaManifestSha256,
    [Parameter(Mandatory)][string]$MediaInputResult,
    [Parameter(Mandatory)][string]$ApprovedMediaArchive,
    [Parameter(Mandatory)][string]$MediaStage,
    [Parameter(Mandatory)][string]$RuntimeRedistRoot,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+(?:\.[0-9]+){2,3}$')][string]$RuntimeVersion,
    [switch]$AllowVerifiedCiBuildPaths
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($SourceCheckout).TrimEnd('\')
$buildRelative = 'out\build\windows-release-reviewed\Release'
$manifestRelative = 'out\v2-final-unsigned-payload.json'
$artifactRelative = 'out\v2-final-unsigned-artifacts'
$stageRelative = 'out\v2-final-unsigned-stage'
$evidenceRelative = 'out\v2-final-unsigned-evidence'
$verifyRelative = 'out\v2-final-unsigned-portable-verification'
$prefix = 'FileManager-1.0.0-release-windows-x64'
$manifestPath = Join-Path $root $manifestRelative
$artifact = Join-Path $root $artifactRelative
$stage = Join-Path $root (Join-Path $stageRelative $prefix)
$evidence = Join-Path $root $evidenceRelative
$msi = Join-Path $artifact "$prefix.msi"
$zip = Join-Path $artifact "$prefix-portable.zip"
$mediaRoot = [IO.Path]::GetFullPath($MediaStage).TrimEnd('\')
$mediaResultPath = [IO.Path]::GetFullPath($MediaInputResult)
$mediaArchivePath = [IO.Path]::GetFullPath($ApprovedMediaArchive)
$redist = [IO.Path]::GetFullPath($RuntimeRedistRoot).TrimEnd('\')

function Assert-OrdinaryChain([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Final unsigned package path crosses a reparse point.'
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
}
if ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\') -ine $root -or
        -not (Test-Path -LiteralPath (Join-Path $root $buildRelative) -PathType Container) -or
        -not (Test-Path -LiteralPath $mediaResultPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $mediaArchivePath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $mediaRoot -PathType Container) -or
        -not (Test-Path -LiteralPath $redist -PathType Container)) {
    throw 'Final unsigned package requires the reviewed build, media and runtime.'
}
foreach ($path in @($root, (Join-Path $root $buildRelative), $mediaResultPath,
        $mediaArchivePath, $mediaRoot, $redist)) {
    Assert-OrdinaryChain $path
}
foreach ($path in @($manifestPath, $artifact, $stage, $evidence, (Join-Path $root $verifyRelative))) {
    if (Test-Path -LiteralPath $path) {
        throw 'Final unsigned package requires fresh manifest, output and evidence paths.'
    }
    Assert-OrdinaryChain (Split-Path -Parent $path)
}
$head = @(& git -C $root rev-parse HEAD 2>$null)
$gitStatus = @(& git -C $root status --porcelain=v1 --untracked-files=all 2>$null)
if ($LASTEXITCODE -ne 0 -or $head.Count -ne 1 -or
        $head[0].Trim() -ine $ExpectedSourceCommit -or $gitStatus.Count -ne 0) {
    throw 'Final unsigned package source checkout differs from the clean approved commit.'
}
$approvedRelative = 'packaging/ffmpeg-1.0-v53-approved-input.json'
$approvedHash = 'AE68D738D51E7B5D07A68BDA0B07E85F33615B1192847BB68FE04CBC7F49E61C'
$archiveRelative = 'packaging/v53-approved-media-inputs.zip'
$archiveHash = '2D0D037BD8B6DB9E9D24A8EFA8FDDD0A7DED197E2041201E933044F1DB0DA588'
if ($ApprovedMediaManifest -cne $approvedRelative -or
        $ExpectedApprovedMediaManifestSha256 -ine $approvedHash -or
        $mediaArchivePath -ine (Join-Path $root $archiveRelative) -or
        (Get-FileHash -LiteralPath $mediaArchivePath -Algorithm SHA256).Hash -ine $archiveHash) {
    throw 'Final unsigned package requires the reviewed V53 manifest and exact media archive.'
}
$approvedRecord = Get-Content -Raw -LiteralPath (Join-Path $root $approvedRelative) | ConvertFrom-Json
$approvedFiles = [ordered]@{
    'avcodec-61.dll' = [string]$approvedRecord.dlls.'avcodec-61.dll'
    'avformat-61.dll' = [string]$approvedRecord.dlls.'avformat-61.dll'
    'avutil-59.dll' = [string]$approvedRecord.dlls.'avutil-59.dll'
    'swresample-5.dll' = [string]$approvedRecord.dlls.'swresample-5.dll'
    'swscale-8.dll' = [string]$approvedRecord.dlls.'swscale-8.dll'
    'Qt6Multimedia.dll' = [string]$approvedRecord.qtMultimediaModuleSha256
    'multimedia/ffmpegmediaplugin.dll' = [string]$approvedRecord.qtMediaPluginSha256
}
$resultItem = Get-Item -LiteralPath $mediaResultPath -Force
if ($resultItem -isnot [IO.FileInfo] -or
        ($resultItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'The retained media input result must be an ordinary file.'
}
$result = Get-Content -Raw -LiteralPath $mediaResultPath | ConvertFrom-Json
$mediaInputResultHash = (Get-FileHash -LiteralPath $mediaResultPath -Algorithm SHA256).Hash
if ($result.schema -cne 'file-manager-approved-retained-media-input-v1' -or
        $result.status -cne 'approved-retained-input' -or
        $result.packagingInputApproved -ne $true -or
        $result.finalReleaseApproved -ne $false -or
        $result.packageBuilt -ne $false -or $result.uploaded -ne $false -or
        $result.sourceRunId -ne 41370308365 -or
        $result.sourceCommit -cne $approvedRecord.sourceCommit -or
        $result.sourceArtifactSha256 -ine
            '1C0F6F177A1D8B26B7DFA3D03FFF84F4EC8325FE125D242032C2DF1976009511' -or
        $result.inputArchiveSha256 -ine $archiveHash -or
        $result.approvedManifestSha256 -ine $approvedHash -or
        $result.mediaProvenanceSha256 -ine $approvedRecord.mediaProvenanceSha256) {
    throw 'The retained V53 media-input record differs from its approved provenance.'
}
$resultPaths = @($result.mediaFiles.PSObject.Properties.Name | Sort-Object)
$stageItems = @(Get-ChildItem -LiteralPath $mediaRoot -Recurse -Force)
if (@($stageItems | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        ($_ -isnot [IO.FileInfo] -and $_ -isnot [IO.DirectoryInfo])
    }).Count -ne 0) {
    throw 'The hosted media stage contains an unsafe item.'
}
$stageDirectories = @($stageItems | Where-Object { $_ -is [IO.DirectoryInfo] } |
    ForEach-Object { [IO.Path]::GetRelativePath($mediaRoot, $_.FullName).Replace('\', '/') })
if ($stageDirectories.Count -ne 1 -or $stageDirectories[0] -cne 'multimedia') {
    throw 'The hosted media stage has unexpected directories.'
}
$stagePaths = @($stageItems | Where-Object { $_ -is [IO.FileInfo] } |
    ForEach-Object { [IO.Path]::GetRelativePath($mediaRoot, $_.FullName).Replace('\', '/') } |
    Sort-Object)
$expectedPaths = @($approvedFiles.Keys | Sort-Object)
if (($resultPaths -join '|') -cne ($expectedPaths -join '|') -or
        ($stagePaths -join '|') -cne ($expectedPaths -join '|')) {
    throw 'The hosted media result or stage differs from seven approved paths.'
}
foreach ($name in $approvedFiles.Keys) {
    if ($result.mediaFiles.PSObject.Properties[$name].Value -ine $approvedFiles[$name] -or
            (Get-FileHash -LiteralPath (Join-Path $mediaRoot $name.Replace('/', '\')) -Algorithm SHA256).Hash -ine
                $approvedFiles[$name]) {
        throw "The staged retained media bytes differ from the V53 approval: $name"
    }
}
$approvedProvenance = Join-Path $root 'packaging\media-provenance-v53-approved.json'
$approvedProvenanceHash = '47E9AE242D40614ECCD39441DC51C149A5CD33DB8DF1B348E9F334A493A6B57E'
if ((Get-FileHash -LiteralPath $approvedProvenance -Algorithm SHA256).Hash -ine $approvedProvenanceHash -or
        $approvedRecord.mediaProvenanceSha256 -ine $approvedProvenanceHash -or
        (Test-Path -LiteralPath (Join-Path $mediaRoot 'manifest'))) {
    throw 'Approved V53 provenance is missing, changed or would overwrite a media-stage item.'
}
New-Item -ItemType Directory -Path (Join-Path $mediaRoot 'manifest') -ErrorAction Stop | Out-Null
Copy-Item -LiteralPath $approvedProvenance -Destination (Join-Path $mediaRoot 'manifest\media-provenance.json') -ErrorAction Stop
. (Join-Path $root 'packaging\ReviewedFfmpeg.ps1')
$mediaArgs = @{
    Workspace=$root; ManifestPath=$ApprovedMediaManifest
    ManifestSha256=$ExpectedApprovedMediaManifestSha256
    RuntimeDirectory=$mediaRoot; RequireReleaseApproval=$true
}
$media = Get-ReviewedFfmpegInputs @mediaArgs
if ($media.Sources.Count -ne 8 -or
        $media.Manifest.sourceCommit -cne '42450e674dd5b25fc3d92b499310a8dcf44b05c8') {
    throw 'Final unsigned package media sources differ from the V53 approval.'
}
$crt = Join-Path $redist (Join-Path $RuntimeVersion 'x64\Microsoft.VC143.CRT')
if (-not (Test-Path -LiteralPath $crt -PathType Container)) {
    throw 'The selected VC143 runtime is missing.'
}
Assert-OrdinaryChain $crt
$observedBuildCues = @()
if ($AllowVerifiedCiBuildPaths) {
    $expectedTemp = Join-Path ([IO.Path]::GetPathRoot($root)) 'a\_temp'
    if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_OS -cne 'Windows' -or
            $root -cnotmatch '^[A-Za-z]:\\a\\File-Manager-1\.0\\File-Manager-1\.0$' -or
            [string]::IsNullOrWhiteSpace($env:GITHUB_WORKSPACE) -or
            [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP) -or
            [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE).TrimEnd('\') -ine $root -or
            [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') -ine $expectedTemp) {
        throw 'The reviewed generic GitHub CI path exception is unavailable.'
    }
    $observedBuildCues = @($root, $expectedTemp)
}
$manifestArgs = @{
    BuildDirectory=$buildRelative; ProductVersion='1.0.0'
    RuntimeRedistRoot=$redist; RuntimeVersion=$RuntimeVersion
    RuntimeCrtFamily='VC143'; ReviewedFfmpegManifest=$ApprovedMediaManifest
    ExpectedReviewedFfmpegManifestSha256=$ExpectedApprovedMediaManifestSha256
    ReviewedFfmpegRuntimeDirectory=$mediaRoot; OutputPath=$manifestRelative
}
$manifest = & (Join-Path $root 'packaging\New-ExpectedPayloadManifest.ps1') @manifestArgs | ConvertFrom-Json
if ($manifest.fileCount -ne 58 -or $manifest.manifestSha256 -cnotmatch '^[A-F0-9]{64}$') {
    throw 'Final unsigned payload manifest differs from the 58-path contract.'
}
$packageArgs = @{
    ProductVersion='1.0.0'; ProductName='File Manager'; Manufacturer='Kiatami'
    ChannelSlug='release'; BuildDirectory=$buildRelative
    OutputDirectory=$artifactRelative; StageDirectory=$stageRelative
    ExpectedPayloadManifest=$manifestRelative; ExpectedManifestSha256=$manifest.manifestSha256
    ReviewedFfmpegManifest=$ApprovedMediaManifest
    ExpectedReviewedFfmpegManifestSha256=$ExpectedApprovedMediaManifestSha256
    ReviewedFfmpegRuntimeDirectory=$mediaRoot; RuntimeRedistRoot=$redist
    ExpectedRuntimeVersion=$RuntimeVersion; RuntimeCrtFamily='VC143'; AcceptWixEula=$true
}
& (Join-Path $root 'packaging\Build-Packages.ps1') @packageArgs | Out-Null
New-Item -ItemType Directory -Path $evidence -ErrorAction Stop | Out-Null
$debugFile = Join-Path $artifact "$prefix.wixpdb"
if (Test-Path -LiteralPath $debugFile -PathType Leaf) {
    Move-Item -LiteralPath $debugFile -Destination (Join-Path $evidence "$prefix.wixpdb")
}
$msiArgs = @{
    Package=$msi; ExpectedVersion='1.0.0'; ExpectedProductName='File Manager'
    ExpectedManufacturer='Kiatami'; AcceptWixEula=$true
}
& (Join-Path $root 'packaging\Verify-MsiPackage.ps1') @msiArgs | Out-Null
$portableArgs = @{
    Archive=$zip; ExpectedVersion='1.0.0'; ExpectedProductName='File Manager'
    ExpectedManufacturer='Kiatami'; ExpectedPayloadManifest=$manifestPath
    ExpectedManifestSha256=$manifest.manifestSha256
    VerificationBase=(Join-Path $root $verifyRelative)
}
& (Join-Path $root 'packaging\Verify-PortablePackage.ps1') @portableArgs | Out-Null
$items = @(Get-ChildItem -LiteralPath $artifact -Force -ErrorAction Stop)
if ($items.Count -ne 2 -or @($items | Where-Object {
            $_ -isnot [IO.FileInfo] -or
            $_.Name -cnotin @("$prefix.msi", "$prefix-portable.zip") -or
            ($_.Attributes -band ([IO.FileAttributes]::ReparsePoint -bor
                [IO.FileAttributes]::Hidden -bor [IO.FileAttributes]::System)) -ne 0
        }).Count -ne 0) {
    throw 'Final unsigned signing-input folder must contain only the MSI and ZIP.'
}
$totalBytes = [long]0
foreach ($item in $items) { $totalBytes += [long]$item.Length }
if ($totalBytes -gt 134217728) {
    throw 'Final unsigned signing-input files exceed the 128 MiB review ceiling.'
}
$privacyArgs = @((Join-Path $root 'tools\Check-ReleaseArtifactPrivacy.py'),
    '--payload-root', $stage, '--payload-manifest', $manifestPath,
    '--manifest-sha256', $manifest.manifestSha256,
    '--artifact-dir', $artifact, '--report', (Join-Path $evidence 'privacy.json'))
foreach ($file in @($msi, $zip)) {
    $privacyArgs += @('--asset',"$([IO.Path]::GetFileName($file))=$((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash)")
}
$cues = @($redist, $env:USERPROFILE, $env:COMPUTERNAME)
if (-not $AllowVerifiedCiBuildPaths) { $cues += @($root, $mediaRoot) }
foreach ($cue in $cues) {
    if (-not [string]::IsNullOrWhiteSpace($cue)) { $privacyArgs += @('--forbidden-cue', $cue) }
}
foreach ($cue in $observedBuildCues) { $privacyArgs += @('--observed-build-cue', $cue) }
$privacyOutput = @(& python @privacyArgs 2> (Join-Path $evidence 'privacy-stderr.txt'))
if ($LASTEXITCODE -ne 0) { throw 'Final unsigned signing-input privacy gate did not pass.' }
$privacy = Get-Content -Raw -LiteralPath (Join-Path $evidence 'privacy.json') | ConvertFrom-Json -AsHashtable
if ($privacy.status -cne 'passed' -or $privacy.payloadCount -ne 58 -or
        $privacy.artifactCount -ne 2 -or $privacy.manifestSha256 -cne $manifest.manifestSha256 -or
        $privacy.uploaded -ne $false -or $privacy.cueLabelsAndCountsOnly.Count -ne 0 -or
        $privacy.cueCount -ne @($cues | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count) {
    throw 'Final unsigned signing-input privacy report is inconsistent.'
}
if ($privacyOutput.Count -ne 1 -or ($privacyOutput[0] | ConvertFrom-Json).status -cne 'passed') {
    throw 'Final unsigned signing-input privacy summary is missing.'
}
if ($AllowVerifiedCiBuildPaths) {
    $expectedObserved = @{
        'FileManager.exe' = @{ cue='cue-1/utf-8'; max=1 }
        'FileManagerPreviewHost.exe' = @{ cue='cue-1/utf-8'; max=1 }
        'avcodec-61.dll' = @{ cue='cue-2/utf-8'; max=11 }
        'avformat-61.dll' = @{ cue='cue-2/utf-8'; max=1 }
        'avutil-59.dll' = @{ cue='cue-2/utf-8'; max=1 }
        'swscale-8.dll' = @{ cue='cue-2/utf-8'; max=1 }
    }
    if ($privacy.observedBuildCueCount -ne 2 -or
            $privacy.observedBuildCueLabelsAndCountsOnly.Count -gt 6) {
        throw 'Unreviewed CI build-path evidence appeared in final signing input.'
    }
    foreach ($finding in $privacy.observedBuildCueLabelsAndCountsOnly.GetEnumerator()) {
        if (-not $expectedObserved.ContainsKey($finding.Key)) {
            throw 'An unreviewed CI build-path file appeared in final signing input.'
        }
        $expected = $expectedObserved[$finding.Key]
        if ($finding.Value.Count -ne 1 -or
                -not $finding.Value.Contains($expected.cue) -or
                [long]$finding.Value[$expected.cue] -lt 1 -or
                [long]$finding.Value[$expected.cue] -gt $expected.max) {
            throw 'An unreviewed CI build-path cue appeared in final signing input.'
        }
    }
} elseif ($privacy.observedBuildCueCount -ne 0 -or
        $privacy.observedBuildCueLabelsAndCountsOnly.Count -ne 0) {
    throw 'Local final signing input unexpectedly allowed a CI build path.'
}
[pscustomobject]@{
    status='unsigned-signing-input-local-only'; sourceCommit=$ExpectedSourceCommit.ToLowerInvariant()
    approvedMediaManifestSha256=$ExpectedApprovedMediaManifestSha256.ToUpperInvariant()
    mediaInputResultSha256=$mediaInputResultHash
    approvedMediaArchiveSha256=$archiveHash
    approvedProvenanceSha256=$approvedProvenanceHash
    payloadManifestSha256=$manifest.manifestSha256; appSha256=$manifest.appSha256
    msiSha256=(Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash
    portableSha256=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    artifactFileCount=2; artifactTotalBytes=$totalBytes; uploaded=$false; signed=$false
} | ConvertTo-Json -Depth 3
