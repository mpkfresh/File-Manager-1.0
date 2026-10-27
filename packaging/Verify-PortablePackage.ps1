#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Archive,
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$ExpectedVersion = '0.8.7.6',
    [string]$ExpectedProductName,
    [string]$ExpectedManufacturer,
    [string]$ExpectedPayloadManifest,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedManifestSha256,
    [string]$VerificationBase,
    [switch]$RequireSignature,
    [ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$ExpectedSignerThumbprint,
    [ValidateRange(1, 600)]
    [int]$ProbeTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseSignatures.ps1')
. (Join-Path $PSScriptRoot 'PayloadInventory.ps1')
if (($RequireSignature -or $ExpectedSignerThumbprint) -and
        (-not $ExpectedPayloadManifest -or -not $ExpectedManifestSha256)) {
    throw 'Signed portable verification requires a hash-pinned approved payload manifest.'
}
if ($ExpectedManifestSha256 -and -not $ExpectedPayloadManifest) {
    throw 'A manifest SHA-256 requires an expected payload manifest.'
}
$versionContract = Get-FileManagerVersionContract $ExpectedVersion
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$outRoot = if ($VerificationBase) {
    [IO.Path]::GetFullPath($VerificationBase)
} else {
    [IO.Path]::GetFullPath((Join-Path $workspace 'out'))
}
$verificationBase = if ($VerificationBase) {
    $outRoot
} else {
    [IO.Path]::GetFullPath((Join-Path $outRoot 'package-verification'))
}
if (-not $verificationBase.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Portable verification directory escaped the workspace.'
}
function Assert-OrdinaryDirectoryChain([string]$Path) {
    $directory = [IO.DirectoryInfo]::new($Path)
    while ($null -ne $directory) {
        if ($directory.Exists -and
                ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Portable verification crosses a reparse-backed directory: $($directory.FullName)"
        }
        $directory = $directory.Parent
    }
}
Assert-OrdinaryDirectoryChain $verificationBase
if (-not (Test-Path -LiteralPath $outRoot)) {
    [IO.Directory]::CreateDirectory($outRoot) | Out-Null
}
if ((Get-Item -LiteralPath $outRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw "Portable verification output root must not be a reparse point: $outRoot"
}
if (Test-Path -LiteralPath $verificationBase) {
    if ((Get-Item -LiteralPath $verificationBase -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Portable verification base must not be a reparse point.'
    }
}
[IO.Directory]::CreateDirectory($verificationBase) | Out-Null
Assert-OrdinaryDirectoryChain $verificationBase
$verificationRoot = Join-Path $verificationBase ([Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($verificationRoot) | Out-Null

try {
    Expand-Archive -LiteralPath $archivePath -DestinationPath $verificationRoot
    $roots = @(Get-ChildItem -LiteralPath $verificationRoot -Force)
    if ($roots.Count -ne 1 -or $roots[0] -isnot [IO.DirectoryInfo]) {
        throw 'The portable archive must contain exactly one top-level directory.'
    }
    $payload = $roots[0].FullName
    Assert-FileManagerPayloadDirectory -Directory $payload `
        -ProductVersion $ExpectedVersion
    if ($ExpectedPayloadManifest) {
        $manifestPath = (Resolve-Path -LiteralPath $ExpectedPayloadManifest).Path
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
        $entries = @($manifest.files)
        Assert-FileManagerPayloadPaths -Paths @($entries | ForEach-Object { [string]$_.path }) `
            -ProductVersion $ExpectedVersion
        if ($manifest.fileCount -ne $entries.Count) {
            throw 'Expected payload manifest file count is inconsistent.'
        }
        foreach ($entry in $entries) {
            if ($entry.sha256 -cnotmatch '^[A-Fa-f0-9]{64}$') {
                throw "Expected payload manifest has an invalid hash for $($entry.path)."
            }
            $extracted = Join-Path $payload ($entry.path.Replace('/', [IO.Path]::DirectorySeparatorChar))
            $item = Get-Item -LiteralPath $extracted -Force
            if ($item -isnot [IO.FileInfo] -or $item.Length -ne [long]$entry.bytes -or
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $extracted).Hash -ine $entry.sha256) {
                throw "Portable payload differs from the approved manifest: $($entry.path)"
            }
        }
        $appEntry = @($entries | Where-Object { $_.path -ceq 'FileManager.exe' })
        if ($appEntry.Count -ne 1 -or $manifest.appSha256 -ine $appEntry[0].sha256) {
            throw 'Expected payload manifest app identity is inconsistent.'
        }
    }
    $required = @(
        'FileManager.exe',
        'FileManagerShellHost.exe',
        'FileManagerPreviewHost.exe',
        'FileManagerContextMenuHost.exe',
        'FileManagerMaintenance.exe',
        'Qt6Core.dll',
        'Qt6Gui.dll',
        'Qt6Multimedia.dll',
        'Qt6Network.dll',
        'Qt6Sql.dll',
        'Qt6Widgets.dll',
        'msvcp140.dll',
        'vcruntime140.dll',
        'vcruntime140_1.dll',
        'platforms\qwindows.dll',
        'sqldrivers\qsqlite.dll',
        'tls\qschannelbackend.dll',
        'licenses\Qt-LICENSE.txt',
        'licenses\FFmpeg-LGPL-2.1.txt',
        'licenses\Signalsmith-MIT.txt',
        'licenses\Mozilla-Public-License-2.0.txt',
        'licenses\Qt-ThirdParty-Attributions.txt',
        'licenses\Windows-SDK-Third-Party-Notices.rtf',
        'sbom\qtbase-6.11.2.cdx.json',
        'sbom\qtsvg-6.11.2.cdx.json',
        'LICENSE',
        'THIRD_PARTY_NOTICES.md',
        'SECURITY.md',
        'PORTABLE_README.txt'
    )
    if ([Version]$ExpectedVersion -ge [Version]'1.0.0') {
        $required += 'manifest\media-provenance.json'
    } else {
        $required += 'sbom\qtmultimedia-6.11.2.cdx.json'
    }
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $payload $relative) -PathType Leaf)) {
            throw "Required portable payload is missing: $relative"
        }
    }

    $forbidden = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object {
        $_.Name -match '(?i)(test|benchmark)' -or $_.Extension -ieq '.pdb' -or
        $_.Name -ieq 'vc_redist.x64.exe'
    })
    if ($forbidden.Count -ne 0) {
        throw "Forbidden development payload entered the archive: $($forbidden.Name -join ', ')"
    }
    $allowedExecutables = @(Get-FileManagerExecutableNames)
    $extraExecutables = @(Get-ChildItem -LiteralPath $payload -Recurse -Filter '*.exe' -File |
        Where-Object Name -NotIn $allowedExecutables)
    if ($extraExecutables.Count -ne 0) {
        throw "Unexpected executable entered the archive: $($extraExecutables.Name -join ', ')"
    }

    $application = Get-Item -LiteralPath (Join-Path $payload 'FileManager.exe')
    $identity = Assert-FileManagerPayloadIdentity -PayloadDirectory $payload `
        -ExpectedVersion $ExpectedVersion -ExpectedProductName $ExpectedProductName `
        -ExpectedManufacturer $ExpectedManufacturer
    $versionInfo = $application.VersionInfo
    $numericVersion = '{0}.{1}.{2}.{3}' -f $versionInfo.FileMajorPart, $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart, $versionInfo.FilePrivatePart
    if ($numericVersion -ne $versionContract.FileVersion) {
        throw "Expected FileManager.exe numeric version $($versionContract.FileVersion); found $numericVersion."
    }
    if ($RequireSignature -or $ExpectedSignerThumbprint) {
        # Verify every app-owned executable before running any extracted code.
        Assert-FileManagerBinarySignatures -PayloadDirectory $payload `
            -ExpectedSignerThumbprint $ExpectedSignerThumbprint | Out-Null
    }
    $probe = New-Object Diagnostics.Process
    try {
        $probe.StartInfo.FileName = $application.FullName
        $probe.StartInfo.Arguments = '--deployment-probe'
        $probe.StartInfo.WorkingDirectory = $payload
        $probe.StartInfo.UseShellExecute = $false
        $probe.StartInfo.CreateNoWindow = $true
        if (-not $probe.Start()) { throw 'Could not start the extracted deployment probe.' }
        if (-not $probe.WaitForExit($ProbeTimeoutSeconds * 1000)) {
            try {
                $probe.Kill($true)
            } catch [InvalidOperationException] {
                # The probe exited between the timeout and cancellation.
            }
            if (-not $probe.WaitForExit(5000)) {
                throw 'The timed-out deployment probe did not stop within five seconds.'
            }
            throw "The extracted deployment probe timed out after $ProbeTimeoutSeconds seconds."
        }
        $probeExit = $probe.ExitCode
    }
    finally {
        $probe.Dispose()
    }
    if ($probeExit -ne 0) {
        throw "The fresh-extraction deployment probe failed with exit code $probeExit."
    }

    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath
    [pscustomobject]@{
        Archive = $archivePath
        Entries = @(Get-ChildItem -LiteralPath $payload -Recurse -File).Count
        FileVersion = $application.VersionInfo.FileVersion
        ProductName = $identity.ProductName
        Manufacturer = $identity.Manufacturer
        SHA256 = $hash.Hash
        DeploymentProbe = 'Passed'
        BinarySignatures = if ($RequireSignature -or $ExpectedSignerThumbprint) {
            'Passed (five application executables)'
        } else { 'Not required (engineering artifact)' }
        PayloadManifest = if ($ExpectedPayloadManifest) { 'Passed (all extracted files)' } else { 'Not supplied' }
    } | Format-List
}
finally {
    if (Test-Path -LiteralPath $verificationRoot) {
        $resolvedRoot = [IO.Path]::GetFullPath($verificationRoot)
        $insideBase = $resolvedRoot.StartsWith(
            $verificationBase + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)
        $rootAttributes = (Get-Item -LiteralPath $resolvedRoot -Force).Attributes
        if (-not $insideBase -or ($rootAttributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Refusing to remove a verification directory outside the owned base.'
        }
        Assert-OrdinaryDirectoryChain $verificationBase
        foreach ($item in @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to remove a verification tree containing a reparse point: $($item.FullName)"
            }
        }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
