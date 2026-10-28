[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$ProductVersion,
    [string]$ProductName,
    [string]$Manufacturer,
    [ValidatePattern('^[a-z0-9]+(?:[.-][a-z0-9]+)*$')]
    [string]$ChannelSlug = 'alpha',
    [string]$BuildDirectory = 'out\build\codex-release4',
    [string]$OutputDirectory = 'package-artifacts',
    [string]$StageDirectory = 'package-staging',
    [string]$ExpectedPayloadManifest,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedManifestSha256,
    [string]$ReviewedFfmpegManifest,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedReviewedFfmpegManifestSha256,
    [string]$ReviewedFfmpegRuntimeDirectory,
    [string]$RuntimeRedistRoot,
    [ValidatePattern('^[0-9]+(?:\.[0-9]+){2,3}$')]
    [string]$ExpectedRuntimeVersion,
    [ValidateSet('VC143', 'VC145')]
    [string]$RuntimeCrtFamily,
    [switch]$SkipMsi,
    [switch]$SkipZip,
    [switch]$AcceptWixEula,
    [switch]$ReviewOnlyPrerelease,
    [switch]$RequireSignedBinaries,
    [ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$ExpectedSignerThumbprint
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseSignatures.ps1')
. (Join-Path $PSScriptRoot 'PayloadInventory.ps1')
. (Join-Path $PSScriptRoot 'ReviewedFfmpeg.ps1')
if ($SkipMsi -and $SkipZip) {
    throw 'Choose at least one output: MSI, portable ZIP, or both.'
}
if ($RequireSignedBinaries -and -not $ExpectedSignerThumbprint) {
    throw 'Final signed packaging requires the expected signer certificate thumbprint.'
}
if ($ReviewOnlyPrerelease -and ($RequireSignedBinaries -or $ExpectedSignerThumbprint -or
        $ChannelSlug -cne 'unsigned-review' -or
        -not $PSBoundParameters.ContainsKey('ChannelSlug'))) {
    throw 'Review-only prerelease requires explicit unsigned-review channel and no signing parameters.'
}
if ($RequireSignedBinaries -and -not $PSBoundParameters.ContainsKey('ChannelSlug')) {
    throw 'Final signed packaging requires an explicit channel slug; do not inherit the alpha default.'
}
if (($RequireSignedBinaries -or $ExpectedSignerThumbprint) -and
        (-not $ExpectedPayloadManifest -or
        -not $ExpectedManifestSha256 -or
        -not $RuntimeRedistRoot -or -not $ExpectedRuntimeVersion -or
        -not $RuntimeCrtFamily)) {
    throw 'Final signed packaging requires a hash-pinned full payload manifest and explicit MSVC runtime root, version and CRT family.'
}
if ($ExpectedManifestSha256 -and -not $ExpectedPayloadManifest) {
    throw 'A manifest SHA-256 requires an expected payload manifest.'
}
if ($ExpectedReviewedFfmpegManifestSha256 -and -not $ReviewedFfmpegManifest) {
    throw 'A reviewed FFmpeg manifest SHA-256 requires its manifest path.'
}
if ($ReviewedFfmpegRuntimeDirectory -and -not $ReviewedFfmpegManifest) {
    throw 'A reviewed FFmpeg runtime directory requires its manifest.'
}

function ConvertTo-WixXml([string]$Value) {
    return [Security.SecurityElement]::Escape($Value)
}

function Get-StableIdentity([string]$Seed) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Seed.ToLowerInvariant())
    $hash = [Security.Cryptography.SHA256]::HashData($bytes)
    $guidBytes = [byte[]]::new(16)
    [Array]::Copy($hash, $guidBytes, 16)
    $guidBytes[6] = ($guidBytes[6] -band 0x0f) -bor 0x50
    $guidBytes[8] = ($guidBytes[8] -band 0x3f) -bor 0x80
    return [pscustomobject]@{
        Id = [Convert]::ToHexString($hash).Substring(0, 24)
        Guid = ([Guid]::new($guidBytes)).ToString().ToUpperInvariant()
    }
}

function New-WixPayloadFragment([string]$PayloadRoot, [string]$Destination) {
    $installerRegistryKey = ConvertTo-WixXml (Get-FileManagerInstallerRegistryKey)
    $builder = [Text.StringBuilder]::new()
    $componentIds = [Collections.Generic.List[string]]::new()
    [void]$builder.AppendLine('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
    [void]$builder.AppendLine('  <Fragment>')
    [void]$builder.AppendLine('    <DirectoryRef Id="INSTALLFOLDER">')

    function Add-Directory([IO.DirectoryInfo]$Directory, [string]$Relative,
            [string]$DirectoryId, [int]$Depth) {
        $indent = ' ' * (6 + ($Depth * 2))
        if ($Depth -gt 0) {
            $directoryIdentity = Get-StableIdentity "directory:$Relative"
            $DirectoryId = "Dir_$($directoryIdentity.Id)"
            $escapedName = ConvertTo-WixXml $Directory.Name
            [void]$builder.AppendLine("$indent<Directory Id=`"$DirectoryId`" Name=`"$escapedName`">")
            $indent += '  '
        }

        foreach ($file in Get-ChildItem -LiteralPath $Directory.FullName -File -Force | Sort-Object Name) {
            $fileRelative = if ($Relative) { "$Relative/$($file.Name)" } else { $file.Name }
            $identity = Get-StableIdentity "file:$fileRelative"
            $componentId = "CmpF_$($identity.Id)"
            $componentIds.Add($componentId)
            $source = ConvertTo-WixXml $file.FullName
            $name = ConvertTo-WixXml $file.Name
            [void]$builder.AppendLine("$indent<Component Id=`"$componentId`" Guid=`"$($identity.Guid)`">")
            $fileId = if ($fileRelative -ieq 'FileManagerMaintenance.exe') {
                'FileManagerMaintenanceExecutable'
            } else {
                "Fil_$($identity.Id)"
            }
            [void]$builder.AppendLine("$indent  <File Id=`"$fileId`" Source=`"$source`" Name=`"$name`" KeyPath=`"no`" />")
            [void]$builder.AppendLine("$indent  <RegistryValue Root=`"HKCU`" Key=`"$installerRegistryKey\Components`" Name=`"$($identity.Id)`" Type=`"integer`" Value=`"1`" KeyPath=`"yes`" />")
            [void]$builder.AppendLine("$indent</Component>")
        }

        $cleanupSeed = if ($Relative) { "cleanup:$Relative" } else { 'cleanup:root' }
        $cleanup = Get-StableIdentity $cleanupSeed
        $cleanupComponent = "CmpD_$($cleanup.Id)"
        $componentIds.Add($cleanupComponent)
        [void]$builder.AppendLine("$indent<Component Id=`"$cleanupComponent`" Guid=`"$($cleanup.Guid)`">")
        [void]$builder.AppendLine("$indent  <RegistryValue Root=`"HKCU`" Key=`"$installerRegistryKey\Directories`" Name=`"$($cleanup.Id)`" Type=`"integer`" Value=`"1`" KeyPath=`"yes`" />")
        [void]$builder.AppendLine("$indent  <RemoveFolder Id=`"Rem_$($cleanup.Id)`" On=`"uninstall`" />")
        [void]$builder.AppendLine("$indent</Component>")

        foreach ($child in Get-ChildItem -LiteralPath $Directory.FullName -Directory -Force | Sort-Object Name) {
            $childRelative = if ($Relative) { "$Relative/$($child.Name)" } else { $child.Name }
            Add-Directory $child $childRelative '' ($Depth + 1)
        }
        if ($Depth -gt 0) {
            $indent = ' ' * (6 + ($Depth * 2))
            [void]$builder.AppendLine("$indent</Directory>")
        }
    }

    Add-Directory (Get-Item -LiteralPath $PayloadRoot) '' 'INSTALLFOLDER' 0
    [void]$builder.AppendLine('    </DirectoryRef>')
    [void]$builder.AppendLine('  </Fragment>')
    [void]$builder.AppendLine('  <Fragment>')
    [void]$builder.AppendLine('    <ComponentGroup Id="PayloadComponents">')
    foreach ($componentId in $componentIds) {
        [void]$builder.AppendLine("      <ComponentRef Id=`"$componentId`" />")
    }
    [void]$builder.AppendLine('    </ComponentGroup>')
    [void]$builder.AppendLine('  </Fragment>')
    [void]$builder.AppendLine('</Wix>')
    [IO.File]::WriteAllText($Destination, $builder.ToString(), [Text.UTF8Encoding]::new($false))
}

function Assert-WorkspacePackagingTarget([string]$Target, [string]$Workspace) {
    $root = [IO.Path]::GetFullPath($Workspace).TrimEnd('\')
    $rootItem = Get-Item -LiteralPath $root -Force
    if ($rootItem -isnot [IO.DirectoryInfo] -or
            ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Packaging workspace must be an ordinary local directory: $root"
    }
    $full = [IO.Path]::GetFullPath($Target)
    if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Packaging target escaped the workspace: $full"
    }
    $relative = $full.Substring($root.Length + 1)
    $current = $root
    foreach ($segment in $relative.Split([IO.Path]::DirectorySeparatorChar,
            [StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $segment
        try {
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        } catch [System.Management.Automation.ItemNotFoundException] {
            continue
        }
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Packaging target crosses a reparse point: $current"
        }
    }
}
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$build = (Resolve-Path -LiteralPath (Join-Path $workspace $BuildDirectory)).Path
Assert-WorkspacePackagingTarget -Target $build -Workspace $workspace
if ($ExpectedPayloadManifest) {
    $manifestPath = (Resolve-Path -LiteralPath (Join-Path $workspace $ExpectedPayloadManifest)).Path
    if (-not $manifestPath.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Expected payload manifest must be inside the File Manager workspace.'
    }
    $manifestItem = Get-Item -LiteralPath $manifestPath -Force
    if ($manifestItem -isnot [IO.FileInfo] -or
            ($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Expected payload manifest must be an ordinary file.'
    }
    if ($ExpectedManifestSha256 -and
            (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash -ine
                $ExpectedManifestSha256) {
        throw 'Expected payload manifest differs from the approved manifest hash.'
    }
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if ($manifest.reviewOnlyPrerelease -ne [bool]$ReviewOnlyPrerelease) {
        throw 'Payload manifest review-only mode does not match this package request.'
    }
    $expectedExecutables = @(Get-FileManagerExecutableNames)
    $entries = @($manifest.files)
    Assert-FileManagerPayloadPaths -Paths @($entries | ForEach-Object { [string]$_.path }) `
        -ProductVersion $ProductVersion
    if ($manifest.fileCount -ne $entries.Count) {
        throw 'Expected payload manifest file count is inconsistent.'
    }
    if ($ReviewedFfmpegManifest -and
            $manifest.reviewedFfmpegManifestSha256 -ine
                $ExpectedReviewedFfmpegManifestSha256) {
        throw 'Expected payload manifest does not pin the reviewed FFmpeg manifest.'
    }
    foreach ($entry in $entries) {
        if ($entry.sha256 -cnotmatch '^[A-Fa-f0-9]{64}$' -or
                [long]$entry.bytes -lt 0) {
            throw "Expected payload manifest has invalid size or hash for $($entry.path)."
        }
    }
    foreach ($name in $expectedExecutables) {
        $manifestMatches = @($entries | Where-Object { $_.path -ieq $name })
        if ($manifestMatches.Count -ne 1 -or $manifestMatches[0].sha256 -cnotmatch '^[A-Fa-f0-9]{64}$') {
            throw "Expected payload manifest needs one valid hash for $name."
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $build $name)).Hash
        if ($actual -ne $manifestMatches[0].sha256.ToUpperInvariant()) {
            throw "Built $name differs from the approved payload manifest; no package was staged."
        }
    }
    $appEntry = @($entries | Where-Object { $_.path -ieq 'FileManager.exe' })[0]
    if ($manifest.appSha256 -notmatch '^[A-Fa-f0-9]{64}$' -or
            $manifest.appSha256 -ne $appEntry.sha256) {
        throw 'Expected payload manifest app identity is inconsistent.'
    }
}
$compiledIdentity = Assert-FileManagerPayloadIdentity -PayloadDirectory $build `
    -ExpectedVersion $ProductVersion -ExpectedProductName $ProductName `
    -ExpectedManufacturer $Manufacturer
$ProductVersion = $compiledIdentity.Version
$versionContract = Get-FileManagerVersionContract $ProductVersion
$ProductName = $compiledIdentity.ProductName
$Manufacturer = $compiledIdentity.Manufacturer
if ($ReviewOnlyPrerelease -and $ProductVersion -cne '1.0.0') {
    throw 'Review-only prerelease mode is limited to version 1.0.0.'
}
if ([Version]$ProductVersion -ge [Version]'1.0.0' -and
        -not $PSBoundParameters.ContainsKey('ChannelSlug')) {
    throw 'Version 1.0 or newer requires an explicit channel slug; do not inherit the alpha default.'
}
$requiresPinnedRuntime = ([Version]$ProductVersion -ge [Version]'1.0.0') -or
    $RequireSignedBinaries -or [bool]$ExpectedSignerThumbprint
$requiresReviewedFfmpeg = $requiresPinnedRuntime
if ($requiresReviewedFfmpeg -and
        (-not $ReviewedFfmpegManifest -or
        -not $ExpectedReviewedFfmpegManifestSha256 -or
        -not $ReviewedFfmpegRuntimeDirectory)) {
    throw 'Version 1.0 or signed packaging requires a hash-pinned, release-approved FFmpeg manifest and isolated runtime directory.'
}
if ($ReviewedFfmpegManifest) {
    if (-not $ExpectedReviewedFfmpegManifestSha256 -or
            -not $ReviewedFfmpegRuntimeDirectory) {
        throw 'Reviewed FFmpeg manifest use requires its hash and isolated runtime directory.'
    }
    $reviewedFfmpeg = Get-ReviewedFfmpegInputs -Workspace $workspace `
        -ManifestPath $ReviewedFfmpegManifest `
        -ManifestSha256 $ExpectedReviewedFfmpegManifestSha256 `
        -RuntimeDirectory $ReviewedFfmpegRuntimeDirectory `
        -RequireReleaseApproval:($requiresReviewedFfmpeg -and -not $ReviewOnlyPrerelease)
    if ($ReviewOnlyPrerelease) {
        $reviewRecord = Get-Content -Raw -LiteralPath (
            Join-Path $workspace $ReviewedFfmpegManifest) | ConvertFrom-Json
        if ($reviewRecord.status -cne 'review-only' -or
            $reviewRecord.releaseApproved -ne $false -or
            $reviewRecord.pluginProvenanceCleared -ne $false -or
            $reviewRecord.moduleProvenanceCleared -ne $false) {
            throw 'Unsigned prerelease review requires the unapproved media candidate.'
        }
    }
}
if ($requiresPinnedRuntime -and
        (-not $ExpectedPayloadManifest -or -not $ExpectedManifestSha256 -or
        -not $RuntimeRedistRoot -or -not $ExpectedRuntimeVersion -or
        -not $RuntimeCrtFamily)) {
    throw 'Version 1.0 or signed packaging requires a hash-pinned payload manifest and explicit MSVC runtime root, version and CRT family.'
}
if ([Version]$ProductVersion -ge [Version]'1.0.0' -and
        $RuntimeCrtFamily -ne 'VC143') {
    throw 'Version 1.0 requires the approved VS2022 VC143 runtime family.'
}
if ($requiresPinnedRuntime -and
        (-not $manifest.runtime -or
        -not [string]$manifest.runtime.redistRoot -or
        $manifest.runtime.version -ne $ExpectedRuntimeVersion -or
        $manifest.runtime.crtFamily -ne $RuntimeCrtFamily -or
        [IO.Path]::GetFullPath($manifest.runtime.redistRoot).TrimEnd('\') -ne
            [IO.Path]::GetFullPath($RuntimeRedistRoot).TrimEnd('\'))) {
    throw 'Expected payload manifest does not pin the selected MSVC runtime inputs.'
}
$output = [IO.Path]::GetFullPath((Join-Path $workspace $OutputDirectory))
$packageName = "FileManager-$ProductVersion-$ChannelSlug-windows-x64"
$stage = [IO.Path]::GetFullPath((Join-Path $workspace (Join-Path $StageDirectory $packageName)))
$zip = Join-Path $output "$packageName-portable.zip"
$msi = Join-Path $output "$packageName.msi"
$payloadWxs = Join-Path (Split-Path -Parent $stage) "$packageName-payload.wxs"
$reviewNotice = Join-Path $output "$packageName-UNSIGNED-REVIEW-ONLY.txt"

$writeTargets = @($output, $stage)
if (-not $SkipZip) { $writeTargets += $zip }
if (-not $SkipMsi) { $writeTargets += @($msi, $payloadWxs) }
if ($ReviewOnlyPrerelease) { $writeTargets += $reviewNotice }
foreach ($target in $writeTargets) {
    Assert-WorkspacePackagingTarget -Target $target -Workspace $workspace
}
$protectedRelease = $RequireSignedBinaries -or [bool]$ExpectedSignerThumbprint -or
    $ReviewOnlyPrerelease
if ($protectedRelease) {
    $releaseTargets = @($stage)
    if (-not $SkipZip) { $releaseTargets += $zip }
    if (-not $SkipMsi) { $releaseTargets += @($msi, $payloadWxs) }
    if ($ReviewOnlyPrerelease) { $releaseTargets += $reviewNotice }
    foreach ($target in $releaseTargets) {
        if (Test-Path -LiteralPath $target) {
            throw "Signed packaging requires a fresh output path: $target"
        }
    }
}
if (Test-Path -LiteralPath $stage) {
    if ($protectedRelease) { throw "Signed packaging requires a fresh output path: $stage" }
    Assert-WorkspacePackagingTarget -Target $stage -Workspace $workspace
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage, $output -Force | Out-Null

$executables = @(Get-FileManagerExecutableNames)
foreach ($file in $executables) {
    Copy-Item -LiteralPath (Join-Path $build $file) -Destination $stage
}
Get-ChildItem -LiteralPath $build -Filter '*.dll' -File |
    Copy-Item -Destination $stage
foreach ($directory in @('generic', 'iconengines', 'imageformats', 'multimedia',
        'networkinformation', 'platforms', 'sqldrivers', 'styles', 'tls', 'licenses', 'sbom')) {
    Copy-Item -LiteralPath (Join-Path $build $directory) -Destination $stage -Recurse
}
foreach ($file in @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'SECURITY.md')) {
    Copy-Item -LiteralPath (Join-Path $build $file) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $workspace 'packaging\PORTABLE_README.txt') -Destination $stage

if ([Version]$ProductVersion -ge [Version]'1.0.0') {
    $vendorMultimediaSbom = Join-Path $stage 'sbom\qtmultimedia-6.11.2.cdx.json'
    if (Test-Path -LiteralPath $vendorMultimediaSbom -PathType Leaf) {
        Remove-Item -LiteralPath $vendorMultimediaSbom -Force
    }
}

# Keep the build tree untouched. Qt deployment may have placed vendor media
# DLLs there; the reviewed media bytes enter only the fresh package stage.
if ($ReviewedFfmpegManifest) {
    foreach ($relative in $reviewedFfmpeg.Sources.Keys) {
        $destination = Join-Path $stage $relative.Replace('/', '\')
        $parent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        Copy-Item -LiteralPath $reviewedFfmpeg.Sources[$relative] `
            -Destination $destination -Force -ErrorAction Stop
    }
}

$redistRoot = if ($RuntimeRedistRoot) {
    [IO.Path]::GetFullPath($RuntimeRedistRoot)
} else {
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC'
}
$crtFamily = if ($RuntimeCrtFamily) { $RuntimeCrtFamily } else { 'VC145' }
$crt = if ($ExpectedRuntimeVersion) {
    Join-Path $redistRoot (Join-Path $ExpectedRuntimeVersion "x64\Microsoft.$crtFamily.CRT")
} else {
    Get-ChildItem -LiteralPath $redistRoot -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\Microsoft.$crtFamily.CRT" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}
if (-not $crt) {
    throw 'The Visual C++ x64 app-local runtime was not found.'
}
if (-not (Test-Path -LiteralPath $crt -PathType Container)) {
    throw "Pinned Visual C++ x64 app-local runtime was not found: $crt"
}
$runtimeNames = @(Get-FileManagerRuntimeNames -ProductVersion $ProductVersion)
if ($runtimeNames.Count -ne 10 -or
        @($runtimeNames | Select-Object -Unique).Count -ne 10) {
    throw "Release payload must name exactly ten distinct app-local runtime DLLs."
}
$runtimeSources = foreach ($runtimeName in $runtimeNames) {
    $runtimeSource = Join-Path $crt $runtimeName
    $runtimeItem = Get-Item -LiteralPath $runtimeSource -Force -ErrorAction Stop
    if ($runtimeItem -isnot [IO.FileInfo] -or
            ($runtimeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Pinned runtime file is missing: $runtimeName"
    }
    [pscustomobject]@{ Name = $runtimeName; Source = $runtimeSource }
}
foreach ($runtime in $runtimeSources) {
    $runtimeSource = $runtime.Source
    $runtimeName = $runtime.Name
    $runtimeStaged = Join-Path $stage $runtimeName
    Copy-Item -LiteralPath $runtimeSource -Destination $runtimeStaged -Force
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $runtimeSource).Hash -ne
            (Get-FileHash -Algorithm SHA256 -LiteralPath $runtimeStaged).Hash) {
        throw "Staged runtime differs from the selected $crtFamily redist: $runtimeName"
    }
}

$unexpectedExecutables = Get-ChildItem -LiteralPath $stage -Filter '*.exe' -File |
    Where-Object Name -NotIn $executables
if ($unexpectedExecutables) {
    throw "Unexpected executable entered the release payload: $($unexpectedExecutables.Name -join ', ')"
}

Assert-FileManagerPayloadDirectory -Directory $stage -ProductVersion $ProductVersion
if ($ReviewedFfmpegManifest) {
    foreach ($relative in $reviewedFfmpeg.Sha256.Keys) {
        $staged = Join-Path $stage $relative.Replace('/', '\')
        if ((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -ine
                $reviewedFfmpeg.Sha256[$relative]) {
            throw "Staged reviewed FFmpeg file differs: $relative"
        }
    }
}
if ($ExpectedPayloadManifest) {
    Assert-FileManagerPayloadPaths -Paths @($entries | ForEach-Object { [string]$_.path }) `
        -ProductVersion $ProductVersion
    foreach ($entry in $entries) {
        $stagedFile = Join-Path $stage ($entry.path.Replace('/', [IO.Path]::DirectorySeparatorChar))
        $stagedItem = Get-Item -LiteralPath $stagedFile -Force
        if ($stagedItem -isnot [IO.FileInfo] -or $stagedItem.Length -ne [long]$entry.bytes -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedFile).Hash -ine $entry.sha256) {
            throw "Staged payload differs from the approved manifest: $($entry.path)"
        }
    }
}
Assert-FileManagerPayloadIdentity -PayloadDirectory $stage `
    -ExpectedVersion $ProductVersion -ExpectedProductName $ProductName `
    -ExpectedManufacturer $Manufacturer | Out-Null
if ($RequireSignedBinaries -or $ExpectedSignerThumbprint) {
    # Inspect the exact staged bytes before producing release artifacts.
    Assert-FileManagerBinarySignatures -PayloadDirectory $stage `
        -ExpectedSignerThumbprint $ExpectedSignerThumbprint | Out-Null
}
if (-not $SkipZip) {
    if ($protectedRelease) {
        if (Test-Path -LiteralPath $zip) { throw "Signed packaging requires a fresh output path: $zip" }
    } else {
        Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    }
    Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
}

if (-not $SkipMsi) {
    if ($protectedRelease -and (Test-Path -LiteralPath $payloadWxs)) {
        throw "Signed packaging requires a fresh output path: $payloadWxs"
    }
    New-WixPayloadFragment $stage $payloadWxs
    if ($protectedRelease) {
        if (Test-Path -LiteralPath $msi) { throw "Signed packaging requires a fresh output path: $msi" }
    } else {
        Remove-Item -LiteralPath $msi -Force -ErrorAction SilentlyContinue
    }
    $wixArguments = @('tool', 'run', 'wix', '--', 'build')
    if ($AcceptWixEula) {
        $wixArguments += @('-acceptEula', 'wix7')
    }
    $wixArguments += @(
        (Join-Path $workspace 'packaging\wix\FileManager.wxs'),
        $payloadWxs,
        '-arch', 'x64',
        '-b', "Payload=$stage",
        '-d', "ProductName=$ProductName",
        '-d', "ProductVersion=$($versionContract.MsiVersion)",
        '-d', "Manufacturer=$Manufacturer",
        '-d', "InstallFolderName=$ProductName",
        '-d', "InstallerRegistryKey=$(Get-FileManagerInstallerRegistryKey)",
        '-d', "BrandingIcon=$(Join-Path $workspace 'resources\branding\app-icon.ico')",
        '-o', $msi)
    & dotnet @wixArguments
    if ($LASTEXITCODE -ne 0) {
        throw "WiX failed with exit code $LASTEXITCODE."
    }
}

$artifactPaths = @()
if (-not $SkipZip) {
    $artifactPaths += $zip
}
if (-not $SkipMsi) {
    $artifactPaths += $msi
}
$artifacts = Get-Item -LiteralPath $artifactPaths
$hashes = foreach ($artifact in $artifacts) {
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $artifact.FullName
    [pscustomobject]@{ Name = $artifact.Name; Length = $artifact.Length; SHA256 = $hash.Hash }
}
$hashes | Format-Table -AutoSize
if ($ReviewOnlyPrerelease) {
    $warning = @(
        'FILE MANAGER 1.0.0 — UNSIGNED REVIEW ONLY'
        'These MSI and portable files are unsigned prerelease review candidates.'
        'They are not the signed 1.0 release and have not been approved for publication.'
        'Verify the exact SHA-256 hashes and review the contained files before any use.'
    ) -join "`r`n"
    [IO.File]::WriteAllText($reviewNotice, $warning + "`r`n",
        [Text.UTF8Encoding]::new($false))
    Write-Output "Review notice SHA-256 $((Get-FileHash -LiteralPath $reviewNotice -Algorithm SHA256).Hash)"
}
