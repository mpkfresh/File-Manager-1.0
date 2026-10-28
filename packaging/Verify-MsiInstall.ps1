[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Package,
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$ExpectedVersion = '0.8.7.6',
    [string]$ProductName = 'File Manager',
    [string]$Manufacturer = 'Kiatami',
    [switch]$ReviewOnlyPrerelease,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedPackageSha256,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedAppSha256,
    [ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$ExpectedMsiSignerThumbprint,
    [string]$ExpectedPayloadManifest,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedManifestSha256,
    [switch]$UseCustomInstallFolder
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageIdentity.ps1')
. (Join-Path $PSScriptRoot 'PayloadInventory.ps1')
$versionContract = Get-FileManagerVersionContract $ExpectedVersion
$installerIdentity = Get-FileManagerInstallerIdentity -ReviewOnlyPrerelease:$ReviewOnlyPrerelease
$otherInstallerIdentity = Get-FileManagerInstallerIdentity `
    -ReviewOnlyPrerelease:(-not $ReviewOnlyPrerelease)
if ($ProductName -cne 'File Manager' -or
        ($ReviewOnlyPrerelease -and $ExpectedVersion -cne '1.0.0')) {
    throw 'The selected installer mode requires the File Manager 1.0 application identity.'
}

if ($ExpectedManifestSha256 -and -not $ExpectedPayloadManifest) {
    throw 'A manifest SHA-256 requires an expected payload manifest.'
}
if ([Version]$ExpectedVersion -ge [Version]'1.0.0' -and
        (-not $ExpectedPayloadManifest -or -not $ExpectedManifestSha256 -or
        -not $ExpectedPackageSha256 -or -not $ExpectedAppSha256 -or
        (-not $ReviewOnlyPrerelease -and -not $ExpectedMsiSignerThumbprint))) {
    throw 'A 1.0-or-newer MSI install check requires pinned MSI, app and payload manifest hashes plus the MSI signer thumbprint.'
}

$payloadManifest = $null
if ($ExpectedPayloadManifest) {
    if (-not $ExpectedPackageSha256 -or -not $ExpectedAppSha256 -or
            -not $ExpectedManifestSha256) {
        throw 'Installed payload verification requires pinned MSI, app and manifest SHA-256 values.'
    }
    $manifestPath = (Resolve-Path -LiteralPath $ExpectedPayloadManifest).Path
    $manifestItem = Get-Item -LiteralPath $manifestPath -Force
    if ($manifestItem -isnot [IO.FileInfo] -or
            ($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Expected payload manifest must be an ordinary local file.'
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash -ine
            $ExpectedManifestSha256) {
        throw 'Expected payload manifest differs from the approved manifest hash.'
    }
    $payloadManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifestEntries = @($payloadManifest.files)
    Assert-FileManagerPayloadPaths -Paths @($manifestEntries | ForEach-Object { [string]$_.path }) `
        -ProductVersion $ExpectedVersion
    if ($payloadManifest.fileCount -ne $manifestEntries.Count) {
        throw 'Expected payload manifest file count is inconsistent.'
    }
    $appEntry = @($manifestEntries | Where-Object { $_.path -ceq 'FileManager.exe' })
    if ($appEntry.Count -ne 1 -or $appEntry[0].sha256 -ine $ExpectedAppSha256 -or
            $payloadManifest.appSha256 -ine $ExpectedAppSha256) {
        throw 'Expected payload manifest app identity differs from the pinned executable.'
    }
    foreach ($entry in $manifestEntries) {
        if ($entry.sha256 -cnotmatch '^[A-Fa-f0-9]{64}$' -or
                [long]$entry.bytes -lt 0) {
            throw "Expected payload manifest has invalid size or hash for $($entry.path)."
        }
    }
}

if ($env:OS -ne 'Windows_NT') {
    throw 'MSI install verification requires Windows.'
}

$packagePath = (Resolve-Path -LiteralPath $Package).Path
$packageSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $packagePath).Hash
if ($ExpectedPackageSha256 -and $packageSha256 -ine $ExpectedPackageSha256) {
    throw 'The MSI bytes differ from the approved package hash.'
}
if ($ExpectedMsiSignerThumbprint) {
    $signature = Get-AuthenticodeSignature -LiteralPath $packagePath
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint -ine $ExpectedMsiSignerThumbprint) {
        throw 'The MSI does not have the approved valid signer on this machine.'
    }
}
$workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$scenario = if ($UseCustomInstallFolder) { 'custom' } else { 'default' }
$runLabel = 'install-' + $scenario + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') +
    '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$logDirectory = Join-Path $workspaceRoot (Join-Path 'out\package-validation\logs' $runLabel)
[IO.Directory]::CreateDirectory($logDirectory) | Out-Null
$installLog = Join-Path $logDirectory 'msi-install.log'
$uninstallLog = Join-Path $logDirectory 'msi-uninstall.log'
$defaultInstallPath = Join-Path $env:LOCALAPPDATA "Programs\$($installerIdentity.InstallFolder)"
$customInstallPath = Join-Path $env:LOCALAPPDATA "Programs\$($installerIdentity.InstallFolder) Acceptance"
$installPath = if ($UseCustomInstallFolder) { $customInstallPath } else { $defaultInstallPath }
$shortcutDirectory = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$($installerIdentity.Name)"
$shortcutPath = Join-Path $shortcutDirectory "$($installerIdentity.Name).lnk"
$installerRegistryPath = 'HKCU:\' + $installerIdentity.RegistryKey
$previewRuntimePath = Join-Path $env:LOCALAPPDATA "$Manufacturer\$ProductName\PreviewRuntime"
$previewFixture = Join-Path $logDirectory 'msi-preview-sandbox-probe.txt'

$existingProducts = @(Get-ChildItem -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall' `
        -ErrorAction SilentlyContinue |
    ForEach-Object {
        $properties = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
        if ($properties.DisplayName -in @($installerIdentity.Name, $otherInstallerIdentity.Name)) {
            $properties
        }
    })
if ($existingProducts.Count -ne 0 -or (Test-Path -LiteralPath $defaultInstallPath) `
        -or (Test-Path -LiteralPath $customInstallPath) `
        -or (Test-Path -LiteralPath $shortcutDirectory) `
        -or (Test-Path -LiteralPath $installerRegistryPath) `
        -or (Test-Path -LiteralPath ('HKCU:\' + $otherInstallerIdentity.RegistryKey)) `
        -or (Test-Path -LiteralPath $previewRuntimePath)) {
    throw "Refusing to replace an existing $ProductName installation or installer-owned state."
}

function Invoke-VerifiedProcess([string]$FilePath, [string]$Arguments,
        [int]$TimeoutSeconds = 180) {
    $process = New-Object Diagnostics.Process
    try {
        $process.StartInfo.FileName = $FilePath
        $process.StartInfo.Arguments = $Arguments
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        if ([IO.Path]::IsPathRooted($FilePath)) {
            $process.StartInfo.WorkingDirectory = Split-Path -Parent $FilePath
        }
        if (-not $process.Start()) { throw "Could not start $FilePath." }
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $script:timedOutPid = $process.Id
            if ($FilePath -ieq 'msiexec.exe') {
                $script:unsafeToCleanup = $true
            }
            else {
                try {
                    & "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F | Out-Null
                    if (-not $process.WaitForExit(5000)) {
                        $script:unsafeToCleanup = $true
                    }
                }
                catch { $script:unsafeToCleanup = $true }
            }
            throw "$FilePath exceeded its $TimeoutSeconds-second limit (PID $($process.Id))."
        }
        return $process.ExitCode
    }
    finally {
        $process.Dispose()
    }
}

function Invoke-Msi([string]$Action, [string]$LogPath) {
    $folderArgument = if ($Action -eq '/i' -and $UseCustomInstallFolder) {
        " INSTALLFOLDER=`"$installPath`""
    } else { '' }
    $arguments = "$Action `"$packagePath`" /qn /norestart$folderArgument /l*v `"$LogPath`""
    $exitCode = Invoke-VerifiedProcess 'msiexec.exe' $arguments
    if ($exitCode -ne 0) {
        throw "Windows Installer action '$Action' failed with exit code $exitCode. See $LogPath"
    }
}

$installed = $false
$payloadVerified = $false
$primaryFailure = $null
$cleanupFailure = $null
$unsafeToCleanup = $false
$timedOutPid = $null
try {
    Invoke-Msi '/i' $installLog
    $installed = $true

    $executable = Join-Path $installPath 'FileManager.exe'
    if (-not (Test-Path -LiteralPath $executable)) {
        throw 'The installed application executable is missing.'
    }
    if ($ExpectedAppSha256 -and
            (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash -ine $ExpectedAppSha256) {
        throw 'The installed application bytes differ from the approved executable hash.'
    }
    if ($payloadManifest) {
        # Compare the actual installed tree, not only MSI table names and sizes.
        # Do this before launching the app so its own runtime files cannot be
        # mistaken for installer payload.
        Assert-FileManagerPayloadDirectory -Directory $installPath `
            -ProductVersion $ExpectedVersion
        foreach ($entry in $manifestEntries) {
            $installedFile = Join-Path $installPath ($entry.path.Replace('/', [IO.Path]::DirectorySeparatorChar))
            $item = Get-Item -LiteralPath $installedFile -Force
            if ($item -isnot [IO.FileInfo] -or $item.Length -ne [long]$entry.bytes -or
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $installedFile).Hash -ine $entry.sha256) {
                throw "Installed MSI payload differs from the approved manifest: $($entry.path)"
            }
        }
        $payloadVerified = $true
    }
    if (-not (Test-Path -LiteralPath $shortcutPath)) {
        throw 'The installed Start menu shortcut is missing.'
    }
    if ($UseCustomInstallFolder -and (Test-Path -LiteralPath $defaultInstallPath)) {
        throw 'The custom install unexpectedly populated the default folder.'
    }
    $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
    if ([IO.Path]::GetFullPath($shortcut.TargetPath) -ine [IO.Path]::GetFullPath($executable)) {
        throw 'The Start menu shortcut targets a different installation.'
    }
    $registeredInstallPath = (Get-ItemProperty -LiteralPath $installerRegistryPath -Name InstallPath).InstallPath
    if ([IO.Path]::GetFullPath($registeredInstallPath).TrimEnd('\') -ine [IO.Path]::GetFullPath($installPath).TrimEnd('\')) {
        throw 'The installer ownership marker does not identify the installed application directory.'
    }

    Assert-FileManagerPayloadIdentity -PayloadDirectory $installPath `
        -ExpectedVersion $ExpectedVersion -ExpectedProductName $ProductName `
        -ExpectedManufacturer $Manufacturer | Out-Null
    $probeExit = Invoke-VerifiedProcess $executable '--deployment-probe' 30
    if ($probeExit -ne 0) {
        throw "The installed deployment probe failed with exit code $probeExit."
    }
    $versionInfo = (Get-Item -LiteralPath $executable).VersionInfo
    $actualVersion = '{0}.{1}.{2}.{3}' -f $versionInfo.FileMajorPart, $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart, $versionInfo.FilePrivatePart
    if ($actualVersion -ne $versionContract.FileVersion) {
        throw "Installed version '$actualVersion' does not match '$ExpectedVersion'."
    }

    [IO.File]::WriteAllText($previewFixture, 'File Manager sandbox deployment fixture')
    $sandboxArguments = "--preview-sandbox-probe `"$previewFixture`""
    $sandboxExit = Invoke-VerifiedProcess $executable $sandboxArguments 30
    if ($sandboxExit -ne 0 -or -not (Test-Path -LiteralPath $previewRuntimePath)) {
        throw "The installed preview sandbox probe failed with exit code " +
            "$sandboxExit, or did not stage its private runtime."
    }
}
catch {
    $primaryFailure = $_
}
finally {
    if ($installed -and -not $unsafeToCleanup) {
        try {
            Invoke-Msi '/x' $uninstallLog
        }
        catch {
            $cleanupFailure = $_
        }
    }
    if (Test-Path -LiteralPath $previewFixture) {
        [IO.File]::Delete($previewFixture)
    }
}

$residue = [ordered]@{
    DefaultInstallFolder = Test-Path -LiteralPath $defaultInstallPath
    CustomInstallFolder = Test-Path -LiteralPath $customInstallPath
    ShortcutFolder = Test-Path -LiteralPath $shortcutDirectory
    InstallerRegistry = Test-Path -LiteralPath $installerRegistryPath
    PreviewRuntime = Test-Path -LiteralPath $previewRuntimePath
}
$residue['ProductRegistration'] = @(Get-ChildItem -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
    ForEach-Object { Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue } |
    Where-Object { $_.DisplayName -eq $installerIdentity.Name }).Count -ne 0
$remaining = @($residue.GetEnumerator() | Where-Object Value | ForEach-Object Key)
$result = [ordered]@{
    Package = $packagePath
    PackageSha256 = $packageSha256
    ExpectedAppSha256 = $ExpectedAppSha256
    ExpectedPayloadManifest = $ExpectedPayloadManifest
    ExpectedManifestSha256 = $ExpectedManifestSha256
    InstalledPayloadVerified = $payloadVerified
    Version = $ExpectedVersion
    Scope = 'Per-user'
    InstallPath = $installPath
    InstallCompleted = $installed
    PrimaryFailure = if ($primaryFailure) { "$primaryFailure" } else { $null }
    CleanupFailure = if ($cleanupFailure) { "$cleanupFailure" } else { $null }
    TimedOutProcessId = $timedOutPid
    CleanupSkipped = $unsafeToCleanup
    Residue = $residue
    Logs = $logDirectory
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $logDirectory 'result.json')
if ($primaryFailure -or $cleanupFailure -or $remaining.Count -ne 0) {
    throw "MSI install lifecycle failed or left app-owned state; see $logDirectory."
}

[pscustomobject]@{
    Package = $packagePath
    Version = $ExpectedVersion
    Scope = 'Per-user'
    InstallPath = $installPath
    DeploymentProbe = 'Passed'
    PreviewSandboxProbe = 'Passed'
    Install = 'Passed'
    Uninstall = 'Passed'
    InstallerResidue = 'None'
    InstallLog = $installLog
    UninstallLog = $uninstallLog
    PackageSha256 = $packageSha256
    InstalledPayloadManifest = if ($payloadVerified) { 'Passed (all installed files)' } else { 'Not supplied' }
    Logs = $logDirectory
}
