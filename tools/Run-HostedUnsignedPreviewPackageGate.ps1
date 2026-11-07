#Requires -Version 7.0
<#
Build and verify one local, unsigned 1.0 preview MSI and portable ZIP from the
same hosted Release build. This never uploads an artifact or approves media for
the supported 1.0 release. All output folders are fresh and workspace-local.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceCheckout,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$ExpectedSourceCommit,
    [Parameter(Mandatory)][string]$MediaBuildResult,
    [Parameter(Mandatory)][string]$MediaStage,
    [Parameter(Mandatory)][string]$RuntimeRedistRoot,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+(?:\.[0-9]+){2,3}$')]
    [string]$RuntimeVersion,
    [switch]$AllowVerifiedCiBuildPaths,
    [string[]]$AdditionalForbiddenCues = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($SourceCheckout).TrimEnd('\')
$buildRelative = 'out\build\windows-release-reviewed\Release'
$mediaResult = [IO.Path]::GetFullPath($MediaBuildResult)
$mediaRoot = [IO.Path]::GetFullPath($MediaStage).TrimEnd('\')
$redist = [IO.Path]::GetFullPath($RuntimeRedistRoot).TrimEnd('\')
$mediaManifestRelative = 'out\v2-review-media.json'
$payloadManifestRelative = 'out\v2-review-payload.json'
$artifactRelative = 'out\v2-review-artifacts'
$stageRelative = 'out\v2-review-stage'
$evidenceRelative = 'out\v2-review-evidence'
$verificationRelative = 'out\v2-portable-verification'
$artifact = Join-Path $root $artifactRelative
$evidence = Join-Path $root $evidenceRelative
$stage = Join-Path $root (Join-Path $stageRelative `
    'FileManager-1.0.0-unsigned-review-windows-x64')
$mediaManifest = Join-Path $root $mediaManifestRelative
$payloadManifest = Join-Path $root $payloadManifestRelative
$prefix = 'FileManager-1.0.0-unsigned-review-windows-x64'
$msi = Join-Path $artifact "$prefix.msi"
$zip = Join-Path $artifact "$prefix-portable.zip"
$notice = Join-Path $artifact "$prefix-UNSIGNED-REVIEW-ONLY.txt"

function Assert-OrdinaryChain([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'V2 review package path crosses a reparse point.'
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
}
if ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\') -ine $root -or
        -not (Test-Path -LiteralPath (Join-Path $root $buildRelative) -PathType Container) -or
        -not (Test-Path -LiteralPath (Join-Path $root 'out') -PathType Container) -or
        -not (Test-Path -LiteralPath $mediaResult -PathType Leaf) -or
        -not (Test-Path -LiteralPath $mediaRoot -PathType Container) -or
        -not (Test-Path -LiteralPath $redist -PathType Container)) {
    throw 'The reviewed build, media result/stage and pinned runtime root must exist.'
}
foreach ($path in @($root, $mediaResult, $mediaRoot, $redist,
        (Join-Path $root $buildRelative))) { Assert-OrdinaryChain $path }
foreach ($path in @($mediaManifest, $payloadManifest, $artifact, $stage,
        $evidence, (Join-Path $root $verificationRelative))) {
    if (Test-Path -LiteralPath $path) {
        throw 'Review package requires fresh media, payload, artifact and evidence paths.'
    }
    Assert-OrdinaryChain (Split-Path -Parent $path)
}
$head = (& git -C $root rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or $head.Trim() -ine $ExpectedSourceCommit) {
    throw 'The V2 source checkout differs from the approved commit.'
}
$crt = Join-Path $redist (Join-Path $RuntimeVersion 'x64\Microsoft.VC143.CRT')
if (-not (Test-Path -LiteralPath $crt -PathType Container)) {
    throw 'The explicitly chosen VC143 runtime version is missing.'
}
Assert-OrdinaryChain $crt

$observedBuildCues = @()
if ($AllowVerifiedCiBuildPaths) {
    $expectedTemp = Join-Path ([IO.Path]::GetPathRoot($root)) 'a\_temp'
    $expectedMediaRoot = Join-Path $expectedTemp 'fm-media-build\seven-media-stage'
    if ($env:GITHUB_ACTIONS -cne 'true' -or $env:RUNNER_OS -cne 'Windows' -or
            $root -cnotmatch '^[A-Za-z]:\\a\\File-Manager-1\.0\\File-Manager-1\.0$' -or
            [string]::IsNullOrWhiteSpace($env:GITHUB_WORKSPACE) -or
            [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP) -or
            [IO.Path]::GetFullPath($env:GITHUB_WORKSPACE).TrimEnd('\') -ine $root -or
            [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') -ine $expectedTemp -or
            $mediaRoot -ine $expectedMediaRoot -or
            $env:GITHUB_SHA -ine $ExpectedSourceCommit) {
        throw 'The verified generic GitHub CI build roots are unavailable.'
    }
    $observedBuildCues = @($root, $expectedTemp)
}

$record = & (Join-Path $root 'tools\New-HostedReviewMediaRecord.ps1') `
    -SourceCheckout $root -MediaBuildResult $mediaResult -MediaStage $mediaRoot `
    -OutputManifest $mediaManifest -ExpectedSourceCommit $ExpectedSourceCommit |
    ConvertFrom-Json
if ($record.status -cne 'review-only' -or $record.mediaCount -ne 7) {
    throw 'The exact seven-file media handoff failed.'
}
$manifest = & (Join-Path $root 'packaging\New-ExpectedPayloadManifest.ps1') `
    -BuildDirectory $buildRelative -ProductVersion '1.0.0' `
    -RuntimeRedistRoot $redist -RuntimeVersion $RuntimeVersion `
    -RuntimeCrtFamily VC143 -ReviewedFfmpegManifest $mediaManifestRelative `
    -ExpectedReviewedFfmpegManifestSha256 $record.manifestSha256 `
    -ReviewedFfmpegRuntimeDirectory $mediaRoot `
    -OutputPath $payloadManifestRelative -ReviewOnlyPrerelease |
    ConvertFrom-Json
if ($manifest.fileCount -ne 58 -or
        $manifest.manifestSha256 -cnotmatch '^[A-F0-9]{64}$') {
    throw 'The review payload manifest differs from the 58-path contract.'
}

& (Join-Path $root 'packaging\Build-Packages.ps1') `
    -ProductVersion '1.0.0' -ProductName 'File Manager' `
    -Manufacturer 'Kiatami' -ChannelSlug 'unsigned-review' `
    -BuildDirectory $buildRelative -OutputDirectory $artifactRelative `
    -StageDirectory $stageRelative `
    -ExpectedPayloadManifest $payloadManifestRelative `
    -ExpectedManifestSha256 $manifest.manifestSha256 `
    -ReviewedFfmpegManifest $mediaManifestRelative `
    -ExpectedReviewedFfmpegManifestSha256 $record.manifestSha256 `
    -ReviewedFfmpegRuntimeDirectory $mediaRoot `
    -RuntimeRedistRoot $redist -ExpectedRuntimeVersion $RuntimeVersion `
    -RuntimeCrtFamily VC143 -ReviewOnlyPrerelease -AcceptWixEula | Out-Null

New-Item -ItemType Directory -Path $evidence -ErrorAction Stop | Out-Null
$debugFile = Join-Path $artifact "$prefix.wixpdb"
if (Test-Path -LiteralPath $debugFile -PathType Leaf) {
    Move-Item -LiteralPath $debugFile -Destination (Join-Path $evidence "$prefix.wixpdb")
}
& (Join-Path $root 'packaging\Verify-MsiPackage.ps1') -Package $msi `
    -ExpectedVersion '1.0.0' -ExpectedProductName 'File Manager (Unsigned Preview)' `
    -ReviewOnlyPrerelease -AcceptWixEula | Out-Null
& (Join-Path $root 'packaging\Verify-PortablePackage.ps1') -Archive $zip `
    -ExpectedVersion '1.0.0' -ExpectedProductName 'File Manager' `
    -ExpectedManufacturer 'Kiatami' -ExpectedPayloadManifest $payloadManifest `
    -ExpectedManifestSha256 $manifest.manifestSha256 `
    -VerificationBase (Join-Path $root $verificationRelative) | Out-Null

$assets = @($msi, $zip, $notice)
if (@($assets | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }).Count -ne 0) {
    throw 'A reviewed unsigned package asset is missing.'
}
$allowedNames = @($assets | ForEach-Object { [IO.Path]::GetFileName($_) })
$artifactItems = @(Get-ChildItem -LiteralPath $artifact -Force -ErrorAction Stop)
if ($artifactItems.Count -ne 3) {
    throw 'The upload directory must contain exactly three review assets.'
}
$artifactTotalBytes = [long]0
$maxArtifactBytes = [long]134217728
foreach ($item in $artifactItems) {
    if ($item -isnot [IO.FileInfo] -or $item.Name -cnotin $allowedNames -or
            ($item.Attributes -band ([IO.FileAttributes]::ReparsePoint -bor
                [IO.FileAttributes]::Hidden -bor [IO.FileAttributes]::System)) -ne 0) {
        throw 'The upload directory contains an unapproved or unsafe item.'
    }
    if ($item.Length -gt ($maxArtifactBytes - $artifactTotalBytes)) {
        throw 'The three review assets exceed the approved 128 MiB upload ceiling.'
    }
    $artifactTotalBytes += [long]$item.Length
}
$privacyArgs = @((Join-Path $root 'tools\Check-ReleaseArtifactPrivacy.py'),
    '--payload-root', $stage, '--payload-manifest', $payloadManifest,
    '--manifest-sha256', $manifest.manifestSha256,
    '--artifact-dir', $artifact, '--report', (Join-Path $evidence 'privacy.json'))
foreach ($file in $assets) {
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    $privacyArgs += @('--asset',"$([IO.Path]::GetFileName($file))=$hash")
}
$cues = if ($AllowVerifiedCiBuildPaths) {
    @($redist, $env:USERPROFILE, $env:COMPUTERNAME) +
        @($AdditionalForbiddenCues)
} else {
    @($root, $mediaRoot, $redist, $env:USERPROFILE, $env:COMPUTERNAME) +
        @($AdditionalForbiddenCues)
}
foreach ($cue in $cues) {
    if (-not [string]::IsNullOrWhiteSpace($cue)) {
        $privacyArgs += @('--forbidden-cue', $cue)
    }
}
foreach ($cue in $observedBuildCues) {
    $privacyArgs += @('--observed-build-cue', $cue)
}
$privacyErrorPath = Join-Path $evidence 'privacy-stderr.txt'
$privacyOutput = @(& python @privacyArgs 2> $privacyErrorPath)
$privacyExit = $LASTEXITCODE
if ($privacyExit -ne 0) {
    # The private report stays on the runner. Print only bounded identifiers and
    # cue indexes so a failed hosted check can be diagnosed without exposing a
    # path, user name, cue value, or any file contents in public job logs.
    $privacyReport = Join-Path $evidence 'privacy.json'
    if ($privacyExit -eq 1 -and (Test-Path -LiteralPath $privacyReport -PathType Leaf)) {
        try {
            $blocked = Get-Content -Raw -LiteralPath $privacyReport |
                ConvertFrom-Json -AsHashtable
            if ($blocked.status -ceq 'blocked' -and
                    $blocked.cueLabelsAndCountsOnly.Count -le 61) {
                $safeEntries = @()
                foreach ($finding in $blocked.cueLabelsAndCountsOnly.GetEnumerator()) {
                    $relativeName = [string]$finding.Key
                    $idBytes = [Security.Cryptography.SHA256]::HashData(
                        [Text.Encoding]::UTF8.GetBytes($relativeName))
                    $safeCues = [ordered]@{}
                    foreach ($cue in $finding.Value.GetEnumerator()) {
                        if ($cue.Key -cmatch '^cue-[1-9][0-9]*/(utf-8|utf-16le)$' -and
                                [long]$cue.Value -gt 0) {
                            $safeCues[$cue.Key] = [long]$cue.Value
                        }
                    }
                    $safeEntries += [ordered]@{
                        fileId = [Convert]::ToHexString($idBytes).Substring(0, 16)
                        kind = if ($relativeName.StartsWith('artifact/')) {
                            'artifact'
                        } else { 'payload' }
                        cues = $safeCues
                    }
                }
                $safeSummary = [ordered]@{
                    flaggedFileCount = $safeEntries.Count
                    findings = $safeEntries
                } | ConvertTo-Json -Depth 5 -Compress
                Write-Host "Privacy gate blocked; safe cue summary: $safeSummary"
            }
        } catch {
            Write-Host 'Privacy gate blocked; its bounded summary was unavailable.'
        }
    }
    throw 'The exact-artifact privacy gate did not pass.'
}
$privacyReport = Get-Content -Raw -LiteralPath (Join-Path $evidence 'privacy.json') |
    ConvertFrom-Json -AsHashtable
if ($privacyOutput.Count -ne 1 -or $privacyReport.status -cne 'passed' -or
        $privacyReport.payloadCount -ne 58 -or $privacyReport.artifactCount -ne 3 -or
        $privacyReport.manifestSha256 -cne $manifest.manifestSha256 -or
        $privacyReport.uploaded -ne $false -or
        $privacyReport.cueCount -ne @($cues | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_)
        }).Count -or $privacyReport.cueLabelsAndCountsOnly.Count -ne 0) {
    throw 'The exact-artifact privacy pass record is missing or inconsistent.'
}
$privacySummary = $privacyOutput[0] | ConvertFrom-Json
if ($privacySummary.status -cne 'passed' -or
        $privacySummary.payloadCount -ne 58 -or
        $privacySummary.artifactCount -ne 3 -or
        $privacySummary.flaggedFiles -ne 0 -or
        $privacySummary.observedBuildFiles -ne
            $privacyReport.observedBuildCueLabelsAndCountsOnly.Count) {
    throw 'The exact-artifact privacy summary differs from its local report.'
}
if ($AllowVerifiedCiBuildPaths) {
    $expectedObserved = @{
        'FileManager.exe' = @{ cue = 'cue-1/utf-8'; max = 1 }
        'FileManagerPreviewHost.exe' = @{ cue = 'cue-1/utf-8'; max = 1 }
        'avcodec-61.dll' = @{ cue = 'cue-2/utf-8'; max = 11 }
        'avformat-61.dll' = @{ cue = 'cue-2/utf-8'; max = 1 }
        'avutil-59.dll' = @{ cue = 'cue-2/utf-8'; max = 1 }
        'swscale-8.dll' = @{ cue = 'cue-2/utf-8'; max = 1 }
    }
    if ($privacyReport.observedBuildCueCount -ne 2 -or
            $privacyReport.observedBuildCueLabelsAndCountsOnly.Count -gt 6) {
        throw 'Unreviewed CI build-path evidence appeared.'
    }
    foreach ($finding in $privacyReport.observedBuildCueLabelsAndCountsOnly.GetEnumerator()) {
        if (-not $expectedObserved.ContainsKey($finding.Key)) {
            throw 'An unreviewed CI build-path file appeared.'
        }
        $expected = $expectedObserved[$finding.Key]
        if ($finding.Value.Count -ne 1 -or
                -not $finding.Value.Contains($expected.cue) -or
                [long]$finding.Value[$expected.cue] -lt 1 -or
                [long]$finding.Value[$expected.cue] -gt $expected.max) {
            throw 'An unreviewed CI build-path cue appeared.'
        }
    }
} elseif ($privacyReport.observedBuildCueCount -ne 0 -or
        $privacyReport.observedBuildCueLabelsAndCountsOnly.Count -ne 0) {
    throw 'Local review unexpectedly allowed a CI build path.'
}

[pscustomobject]@{
    status = 'local-review-only'
    sourceCommit = $ExpectedSourceCommit.ToLowerInvariant()
    mediaManifestSha256 = $record.manifestSha256
    payloadManifestSha256 = $manifest.manifestSha256
    appSha256 = $manifest.appSha256
    msiSha256 = (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash
    portableSha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    noticeSha256 = (Get-FileHash -LiteralPath $notice -Algorithm SHA256).Hash
    artifactFileCount = $artifactItems.Count
    artifactTotalBytes = $artifactTotalBytes
    observedBuildFiles = $privacySummary.observedBuildFiles
    uploaded = $false
} | ConvertTo-Json -Depth 3
