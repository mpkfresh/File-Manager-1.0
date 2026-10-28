#Requires -Version 7.0
<#
Records the exact signed build, portable readme, pinned MSVC runtime inputs,
and reviewed Qt/FFmpeg media bytes for Build-Packages.ps1. Run only within an
approved final-package gate. This does not sign, package, install or establish
redistribution rights.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$ProductVersion = '1.0.0',
    [Parameter(Mandatory)][string]$RuntimeRedistRoot,
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9]+(?:\.[0-9]+){2,3}$')]
    [string]$RuntimeVersion,
    [Parameter(Mandatory)][ValidateSet('VC143')]
    [string]$RuntimeCrtFamily,
    [Parameter(Mandatory)][string]$ReviewedFfmpegManifest,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedReviewedFfmpegManifestSha256,
    [Parameter(Mandatory)][string]$ReviewedFfmpegRuntimeDirectory,
    [Parameter(Mandatory)][string]$OutputPath,
    [switch]$ReviewOnlyPrerelease
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PayloadInventory.ps1')
. (Join-Path $PSScriptRoot 'ReviewedFfmpeg.ps1')

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
function Resolve-WorkspaceChild([string]$path) {
    if ([IO.Path]::IsPathRooted($path)) { throw "Expected a workspace-relative path: $path" }
    $full = [IO.Path]::GetFullPath((Join-Path $workspace $path))
    if (-not $full.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped the workspace: $full"
    }
    return $full
}
function Assert-OrdinaryAncestors([string]$path) {
    $item = [IO.DirectoryInfo]::new($path)
    while ($null -ne $item) {
        if ($item.Exists -and
                ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-backed manifest output is not allowed: $($item.FullName)"
        }
        $item = $item.Parent
    }
}

$build = Resolve-WorkspaceChild $BuildDirectory
if (-not (Test-Path -LiteralPath $build -PathType Container)) {
    throw "Build directory is missing: $build"
}
$reviewedFfmpeg = Get-ReviewedFfmpegInputs -Workspace $workspace `
    -ManifestPath $ReviewedFfmpegManifest `
    -ManifestSha256 $ExpectedReviewedFfmpegManifestSha256 `
    -RuntimeDirectory $ReviewedFfmpegRuntimeDirectory `
    -RequireReleaseApproval:(-not $ReviewOnlyPrerelease)
if ($ReviewOnlyPrerelease) {
    $reviewRecord = Get-Content -Raw -LiteralPath (Resolve-WorkspaceChild $ReviewedFfmpegManifest) |
        ConvertFrom-Json
    if ($reviewRecord.status -cne 'review-only' -or
        $reviewRecord.releaseApproved -ne $false -or
        $reviewRecord.pluginProvenanceCleared -ne $false -or
        $reviewRecord.moduleProvenanceCleared -ne $false -or
        $ProductVersion -cne '1.0.0') {
        throw 'Unsigned prerelease review requires the unapproved 1.0.0 media candidate.'
    }
}
if (-not [IO.Path]::IsPathRooted($RuntimeRedistRoot)) {
    throw 'Runtime redistributable root must be an absolute path.'
}
$redistRoot = [IO.Path]::GetFullPath($RuntimeRedistRoot).TrimEnd('\')
$crt = Join-Path $redistRoot (Join-Path $RuntimeVersion "x64\Microsoft.$RuntimeCrtFamily.CRT")
if (-not (Test-Path -LiteralPath $crt -PathType Container)) {
    throw "Pinned runtime directory is missing: $crt"
}
$output = Resolve-WorkspaceChild $OutputPath
if (Test-Path -LiteralPath $output) { throw "Manifest already exists: $output" }
$outputParent = Split-Path -Parent $output
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    throw "Manifest parent directory is missing: $outputParent"
}
Assert-OrdinaryAncestors $outputParent

$paths = @(Get-FileManagerPayloadPaths -ProductVersion $ProductVersion)
Assert-FileManagerPayloadPaths -Paths $paths -ProductVersion $ProductVersion
$runtimeNames = @(Get-FileManagerRuntimeNames -ProductVersion $ProductVersion)
$files = foreach ($relative in $paths) {
    $source = if ($relative -in $runtimeNames) {
        Join-Path $crt $relative
    } elseif ($reviewedFfmpeg.Sources.Contains($relative)) {
        $reviewedFfmpeg.Sources[$relative]
    } elseif ($relative -eq 'PORTABLE_README.txt') {
        Join-Path $workspace 'packaging\PORTABLE_README.txt'
    } else {
        Join-Path $build ($relative.Replace('/', [IO.Path]::DirectorySeparatorChar))
    }
    $item = Get-Item -LiteralPath $source -Force
    if ($item -isnot [IO.FileInfo] -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Expected an ordinary payload source file: $relative"
    }
    [ordered]@{
        path = $relative
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    }
}
$app = @($files | Where-Object { $_.path -ceq 'FileManager.exe' })
if ($app.Count -ne 1) { throw 'Expected one FileManager.exe in the payload contract.' }
$report = [ordered]@{
    purpose = if ($ReviewOnlyPrerelease) {
        'Expected unsigned review prerelease inputs; not release or publication approval'
    } else {
        'Expected final payload inputs; not signing or publication approval'
    }
    reviewOnlyPrerelease = [bool]$ReviewOnlyPrerelease
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    appSha256 = $app[0].sha256
    runtime = [ordered]@{
        redistRoot = $redistRoot
        version = $RuntimeVersion
        crtFamily = $RuntimeCrtFamily
    }
    reviewedFfmpegManifestSha256 = $ExpectedReviewedFfmpegManifestSha256.ToUpperInvariant()
    fileCount = $files.Count
    files = @($files)
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $output -Encoding utf8NoBOM
[ordered]@{
    manifest = $output
    fileCount = $files.Count
    appSha256 = $app[0].sha256
    manifestSha256 = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
} | ConvertTo-Json -Depth 3
