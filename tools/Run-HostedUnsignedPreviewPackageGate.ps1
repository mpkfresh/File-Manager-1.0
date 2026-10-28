#Requires -Version 7.0
<#
Build and verify one local, unsigned 1.0 preview MSI and portable ZIP from the
same hosted Release build. This never uploads an artifact or approves media for
the final signed release. All output folders are fresh and workspace-local.
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
$privacyArgs = @((Join-Path $root 'tools\Check-ReleaseArtifactPrivacy.py'),
    '--payload-root', $stage, '--payload-manifest', $payloadManifest,
    '--manifest-sha256', $manifest.manifestSha256,
    '--artifact-dir', $artifact, '--report', (Join-Path $evidence 'privacy.json'))
foreach ($file in $assets) {
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    $privacyArgs += @('--asset',"$([IO.Path]::GetFileName($file))=$hash")
}
$cues = @($root, $mediaRoot, $redist, $env:USERPROFILE, $env:COMPUTERNAME) +
    @($AdditionalForbiddenCues)
foreach ($cue in $cues) {
    if (-not [string]::IsNullOrWhiteSpace($cue)) {
        $privacyArgs += @('--forbidden-cue', $cue)
    }
}
& python @privacyArgs | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'The exact-artifact privacy gate did not pass.' }

[pscustomobject]@{
    status = 'local-review-only'
    sourceCommit = $ExpectedSourceCommit.ToLowerInvariant()
    mediaManifestSha256 = $record.manifestSha256
    payloadManifestSha256 = $manifest.manifestSha256
    appSha256 = $manifest.appSha256
    msiSha256 = (Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash
    portableSha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    noticeSha256 = (Get-FileHash -LiteralPath $notice -Algorithm SHA256).Hash
    uploaded = $false
} | ConvertTo-Json -Depth 3
