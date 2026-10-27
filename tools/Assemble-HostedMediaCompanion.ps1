#Requires -Version 7.0
<#
Reconstruct the reviewed media source companion from repository text and
three hash-pinned upstream archives on a fresh Windows build host. Prepared
but unrun; execution and network requests require direct owner approval.
No app build, package, artifact upload or publication occurs here.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TextDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$textRoot = [IO.Path]::GetFullPath($TextDirectory).TrimEnd('\')
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$report = "$output.assembly.json"
$checksumListHash = '5123D85407AD0E077A627945E22F220F4D2778DEE1459C610ECB736BA7F66D13'
$textFiles = @(
    'checksums.sha256', 'README.md', 'THIRD_PARTY_NOTICES.md',
    'build/media-source-and-relink.md',
    'licenses/FFmpeg-LGPL-2.1.txt',
    'licenses/Qt-ThirdParty-Attributions.txt',
    'licenses/Signalsmith-MIT.txt',
    'patches/qt-ffmpeg-attribution.patch',
    'patches/zconf.h.patch'
)
$archives = @(
    [pscustomobject]@{
        Name = 'ffmpeg-n7.1.5.tar.gz'
        Url = 'https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.5.tar.gz'
        Sha256 = 'E3963A50831C985933E1A625ED566EC4C7ADB5C012C34FA9F84438E1D61BDACC'
        Bytes = 15931570L
    },
    [pscustomobject]@{
        Name = 'qtmultimedia-everywhere-src-6.11.2.tar.xz'
        Url = 'https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules/qtmultimedia-everywhere-src-6.11.2.tar.xz'
        Sha256 = '967B5E02EC6B793CDB360622CD6E703132836AF983208D678DAE4B50F109CD9F'
        Bytes = 10255120L
    },
    [pscustomobject]@{
        Name = 'zlib-1.3.1.tar.gz'
        Url = 'https://zlib.net/fossils/zlib-1.3.1.tar.gz'
        Sha256 = '9A93B2B7DFDAC77CEBA5A558A580E74667DD6FEDE4585B91EEFB60F03B72DF23'
        Bytes = 1512791L
    }
)

function Assert-OrdinaryAncestors([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Companion path crosses a reparse point: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
}

function Assert-Hash([string]$Path, [string]$Expected) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -cne $Expected) {
        throw "Media companion file is missing or changed: $($item.Name)"
    }
}

if (-not [IO.Path]::IsPathFullyQualified($TextDirectory) -or
        -not [IO.Path]::IsPathFullyQualified($OutputDirectory) -or
        -not (Test-Path -LiteralPath $textRoot -PathType Container) -or
        (Test-Path -LiteralPath $output) -or
        (Test-Path -LiteralPath $report) -or
        $output -eq [IO.Path]::GetPathRoot($output)) {
    throw 'Expected an existing ordinary text folder and a fresh output folder.'
}
Assert-OrdinaryAncestors $textRoot
Assert-OrdinaryAncestors $output
Assert-OrdinaryAncestors $report
if ($output.StartsWith($textRoot + '\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Companion output must be outside the repository text tree.'
}
Assert-Hash (Join-Path $textRoot 'checksums.sha256') $checksumListHash
$checksums = [ordered]@{}
foreach ($line in Get-Content -LiteralPath (Join-Path $textRoot 'checksums.sha256')) {
    if ($line -cnotmatch '^([A-F0-9]{64})  ([A-Za-z0-9./-]+)$') {
        throw 'The media companion checksum format changed.'
    }
    if ($checksums.Contains($Matches[2])) { throw 'Duplicate media companion path.' }
    $checksums[$Matches[2]] = $Matches[1]
}
if ($checksums.Count -ne 11) {
    throw 'The media companion must contain exactly eleven listed files.'
}
$textItems = @(Get-ChildItem -LiteralPath $textRoot -Recurse -Force)
if (@($textItems | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    }).Count -ne 0) {
    throw 'Repository media text contains a reparse-backed item.'
}
$actualText = @($textItems |
    Where-Object { $_ -is [IO.FileInfo] } |
    ForEach-Object {
        [IO.Path]::GetRelativePath($textRoot, $_.FullName).Replace('\', '/')
    } | Sort-Object)
if (($actualText -join '|') -cne (@($textFiles | Sort-Object) -join '|')) {
    throw 'The repository text folder has an extra or missing file.'
}
foreach ($relative in $textFiles) {
    if ($relative -ne 'checksums.sha256') {
        Assert-Hash (Join-Path $textRoot $relative.Replace('/', '\')) `
            $checksums[$relative]
    }
}
foreach ($archive in $archives) {
    if ($checksums["source/$($archive.Name)"] -cne $archive.Sha256) {
        throw 'The reviewed archive identity differs from its source URL pin.'
    }
}

[IO.Directory]::CreateDirectory($output) | Out-Null
foreach ($relative in $textFiles) {
    $target = Join-Path $output $relative.Replace('/', '\')
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) |
        Out-Null
    Copy-Item -LiteralPath (Join-Path $textRoot $relative.Replace('/', '\')) `
        -Destination $target -ErrorAction Stop
}
$sourceFolder = Join-Path $output 'source'
[IO.Directory]::CreateDirectory($sourceFolder) | Out-Null
$downloaded = @()
foreach ($archive in $archives) {
    $target = Join-Path $sourceFolder $archive.Name
    Invoke-WebRequest -Uri $archive.Url -OutFile $target `
        -MaximumRedirection 5 -TimeoutSec 600 -ErrorAction Stop
    $item = Get-Item -LiteralPath $target -Force
    if ($item -isnot [IO.FileInfo] -or $item.Length -ne $archive.Bytes) {
        throw "Official media source length mismatch: $($archive.Name)"
    }
    Assert-Hash $target $archive.Sha256
    $downloaded += [ordered]@{
        name = $archive.Name
        bytes = $item.Length
        sha256 = $archive.Sha256
        url = $archive.Url
    }
}
foreach ($entry in $checksums.GetEnumerator()) {
    Assert-Hash (Join-Path $output $entry.Key.Replace('/', '\')) $entry.Value
}
[ordered]@{
    schema = 'file-manager-media-companion-assembly-v1'
    status = 'assembled-unreviewed'
    checksumListSha256 = $checksumListHash
    listedFileCount = $checksums.Count
    archives = $downloaded
    appBuilt = $false
    packageBuilt = $false
    uploaded = $false
} | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath $report -Encoding utf8NoBOM
Write-Output 'Assembled hash-pinned media companion locally; no build or upload.'
