[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LowerPackage,
    [Parameter(Mandatory = $true)]
    [string]$HigherPackage,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedLowerPackageSha256,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedHigherPackageSha256,
    [Parameter(Mandatory = $true)]
    [string]$EvidenceRoot,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedComputerName,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedCurrentUserSid,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$LowerVersion,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$HigherVersion,
    [string]$ProductName = 'File Manager',
    [string]$Manufacturer = 'Kiatami',
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedLowerAppSha256,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedHigherAppSha256,
    [switch]$UseCustomInstallFolder
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT' -or
        $env:COMPUTERNAME -ine $ExpectedComputerName -or
        [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ine
            $ExpectedCurrentUserSid) {
    throw 'The disposable Windows machine or user does not match the approved fixture.'
}
. (Join-Path $PSScriptRoot 'PackageIdentity.ps1')
Assert-FileManagerMajorUpgradeVersions -LowerVersion $LowerVersion -HigherVersion $HigherVersion
if ($UseCustomInstallFolder -and (-not $ExpectedLowerAppSha256 -or -not $ExpectedHigherAppSha256)) {
    throw 'Custom-folder upgrade verification requires both exact installed app SHA-256 values.'
}
$lowerVersionContract = Get-FileManagerVersionContract $LowerVersion
$higherVersionContract = Get-FileManagerVersionContract $HigherVersion
$lowerPath = (Resolve-Path -LiteralPath $LowerPackage).Path
$higherPath = (Resolve-Path -LiteralPath $HigherPackage).Path
Assert-FileManagerOrdinaryPath -LiteralPath $lowerPath
Assert-FileManagerOrdinaryPath -LiteralPath $higherPath
foreach ($package in @(
        @{ Path = $lowerPath; ExpectedSha256 = $ExpectedLowerPackageSha256 },
        @{ Path = $higherPath; ExpectedSha256 = $ExpectedHigherPackageSha256 })) {
    $item = Get-Item -LiteralPath $package.Path -Force
    if ($item -isnot [IO.FileInfo] -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-FileHash -LiteralPath $package.Path -Algorithm SHA256).Hash -ine
                $package.ExpectedSha256) {
        throw "MSI fixture is missing, reparse-backed or differs from its approved SHA-256: $($package.Path)"
    }
}
$evidenceRootPath = (Resolve-Path -LiteralPath $EvidenceRoot).Path
$evidenceRootItem = Get-Item -LiteralPath $evidenceRootPath -Force
if ($evidenceRootItem -isnot [IO.DirectoryInfo]) {
    throw 'The named evidence root must be an existing directory.'
}
Assert-FileManagerOrdinaryPath -LiteralPath $evidenceRootPath
$scenario = if ($UseCustomInstallFolder) { 'custom' } else { 'default' }
$runLabel = 'upgrade-' + $scenario + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') +
    '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$logDirectory = Join-Path $evidenceRootPath $runLabel
[IO.Directory]::CreateDirectory($logDirectory) | Out-Null
$defaultInstallPath = Join-Path $env:LOCALAPPDATA "Programs\$ProductName"
$customInstallPath = Join-Path $env:LOCALAPPDATA "Programs\$ProductName Acceptance"
$conflictingInstallPath = Join-Path $env:LOCALAPPDATA "Programs\$ProductName Upgrade Conflict"
$installPath = if ($UseCustomInstallFolder) { $customInstallPath } else { $defaultInstallPath }
$initialFolder = if ($UseCustomInstallFolder) { $customInstallPath } else { '' }
$conflictingRequest = if ($UseCustomInstallFolder) { $conflictingInstallPath } else { '' }
$shortcutDirectory = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$ProductName"
$shortcutPath = Join-Path $shortcutDirectory "$ProductName.lnk"
$installerRegistryPath = 'HKCU:\' + (Get-FileManagerInstallerRegistryKey)
$previewRuntimePath = Join-Path $env:LOCALAPPDATA "$Manufacturer\$ProductName\PreviewRuntime"
$previewFixture = Join-Path $logDirectory 'upgrade-preview-sandbox-probe.txt'
if (-not ('FileManagerMsiNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class FileManagerMsiNative {
    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    public static extern int MsiQueryProductState(string productCode);
}
'@
}

function Read-MsiProperty([string]$PackagePath, [string]$Name) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null,
        $installer, @($PackagePath, 0))
    $q = [char]96
    $sql = "SELECT ${q}Value${q} FROM ${q}Property${q} WHERE ${q}Property${q}='$Name'"
    $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($sql))
    try {
        $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if ($null -eq $record) { throw "MSI property '$Name' is missing." }
        return $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @(1))
    }
    finally {
        $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
    }
}

$lowerProductCode = Read-MsiProperty $lowerPath 'ProductCode'
$higherProductCode = Read-MsiProperty $higherPath 'ProductCode'
$expectedUpgradeCode = '5429857B-08FB-4F84-B33F-25C1B23692CA'
$lowerUpgradeCode = Read-MsiProperty $lowerPath 'UpgradeCode'
$higherUpgradeCode = Read-MsiProperty $higherPath 'UpgradeCode'
if ($lowerProductCode -eq $higherProductCode -or
        ($lowerUpgradeCode -replace '[{}]', '') -ine $expectedUpgradeCode -or
        ($higherUpgradeCode -replace '[{}]', '') -ine $expectedUpgradeCode -or
        (Read-MsiProperty $lowerPath 'ProductName') -ne $ProductName -or
        (Read-MsiProperty $higherPath 'ProductName') -ne $ProductName -or
        (Read-MsiProperty $lowerPath 'Manufacturer') -ne $Manufacturer -or
        (Read-MsiProperty $higherPath 'Manufacturer') -ne $Manufacturer) {
    throw 'MSI product identity or upgrade relationship differs from the requested File Manager fixtures.'
}
if ((Read-MsiProperty $lowerPath 'ProductVersion') -ne $lowerVersionContract.MsiVersion `
        -or (Read-MsiProperty $higherPath 'ProductVersion') -ne $higherVersionContract.MsiVersion) {
    throw 'The MSI fixture versions do not match the requested upgrade test.'
}

function Get-ProductState([string]$ProductCode) {
    return [FileManagerMsiNative]::MsiQueryProductState($ProductCode)
}

$existingProducts = @(Get-ChildItem -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
    ForEach-Object {
        $properties = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
        if ($properties.DisplayName -eq $ProductName) { $properties }
    })
if ($existingProducts.Count -ne 0 -or (Get-ProductState $lowerProductCode) -eq 5 `
        -or (Get-ProductState $higherProductCode) -eq 5 `
        -or ((Test-Path -LiteralPath $defaultInstallPath) -or (Test-Path -LiteralPath $customInstallPath) -or (Test-Path -LiteralPath $conflictingInstallPath)) `
        -or (Test-Path -LiteralPath $shortcutDirectory) `
        -or (Test-Path -LiteralPath $installerRegistryPath) `
        -or (Test-Path -LiteralPath $previewRuntimePath)) {
    throw "Refusing to replace existing $ProductName installation or app-owned sandbox state."
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
                    $process.Kill($true)
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
    finally { $process.Dispose() }
}

function Invoke-Msi([string]$Action, [string]$PackagePath, [string]$LogName, [string]$RequestedFolder = '') {
    $logPath = Join-Path $logDirectory $LogName
    $folderArgument = if ($RequestedFolder) { ' INSTALLFOLDER="' + $RequestedFolder + '"' } else { '' }
    $arguments = "$Action `"$PackagePath`" /qn /norestart$folderArgument /l*v `"$logPath`""
    $exitCode = Invoke-VerifiedProcess 'msiexec.exe' $arguments
    return [pscustomobject]@{ ExitCode = $exitCode; Log = $logPath }
}

function Assert-InstalledLocation([string]$Phase, [string]$ExpectedAppSha256) {
    $executable = Join-Path $installPath 'FileManager.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "$($Phase): installed executable is missing from the expected folder."
    }
    if ($ExpectedAppSha256 -and
            (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash -ne $ExpectedAppSha256) {
        throw "$($Phase): installed application bytes differ from the expected package payload."
    }
    $stored = (Get-ItemProperty -LiteralPath $installerRegistryPath -Name InstallPath).InstallPath
    if ([IO.Path]::GetFullPath($stored).TrimEnd('\') -ine [IO.Path]::GetFullPath($installPath).TrimEnd('\')) {
        throw "$($Phase): installer marker changed the installation folder."
    }
    if (-not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) {
        throw "$($Phase): Start menu shortcut is missing."
    }
    $target = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath).TargetPath
    if ([IO.Path]::GetFullPath($target) -ine [IO.Path]::GetFullPath($executable)) {
        throw "$($Phase): Start menu shortcut targets a different folder."
    }
    if ($UseCustomInstallFolder -and ((Test-Path -LiteralPath $defaultInstallPath) -or
            (Test-Path -LiteralPath $conflictingInstallPath))) {
        throw "$($Phase): default or conflicting install folder was created."
    }
}

$primaryFailure = $null
$unsafeToCleanup = $false
$timedOutPid = $null
$cleanupFailures = [Collections.Generic.List[string]]::new()
try {
    $lowerInstall = Invoke-Msi '/i' $lowerPath 'lower-install.log' $initialFolder
    if ($lowerInstall.ExitCode -ne 0) {
        throw "Lower-version install failed with exit code $($lowerInstall.ExitCode)."
    }
    $executable = Join-Path $installPath 'FileManager.exe'
    if (-not (Test-Path -LiteralPath $executable)) {
        throw 'The lower-version installed executable is missing.'
    }
    if ($UseCustomInstallFolder) { Assert-InstalledLocation 'lower install' $ExpectedLowerAppSha256 }
    [IO.File]::WriteAllText($previewFixture, 'File Manager upgrade sandbox fixture')
    $probeArguments = "--preview-sandbox-probe `"$previewFixture`""
    $lowerProbeExit = Invoke-VerifiedProcess $executable $probeArguments 30
    if ($lowerProbeExit -ne 0 -or -not (Test-Path -LiteralPath $previewRuntimePath)) {
        throw 'The lower-version installed sandbox did not activate successfully.'
    }

    if ($UseCustomInstallFolder) {
        $repair = Invoke-Msi '/fa' $lowerPath 'lower-conflicting-repair.log' $conflictingInstallPath
        if ($repair.ExitCode -ne 0) {
            throw "Custom-folder repair failed with exit code $($repair.ExitCode)."
        }
        Assert-InstalledLocation 'lower repair' $ExpectedLowerAppSha256

        $savedLocation = (Get-ItemProperty -LiteralPath $installerRegistryPath -Name InstallPath).InstallPath
        Remove-ItemProperty -LiteralPath $installerRegistryPath -Name InstallPath
        try {
            $missingMarker = Invoke-Msi '/i' $higherPath 'missing-marker-refusal.log' $conflictingInstallPath
            $refusalLog = [IO.File]::ReadAllText($missingMarker.Log)
            if ($missingMarker.ExitCode -ne 1603 -or
                    $refusalLog -notmatch 'existing installation folder could not be recovered') {
                throw "Missing-marker refusal evidence is invalid: $($missingMarker.ExitCode)."
            }
            if ((Get-ProductState $lowerProductCode) -ne 5 -or
                    (Get-ProductState $higherProductCode) -eq 5) {
                throw 'Missing-marker refusal changed the registered product.'
            }
            $oldExecutable = Join-Path $installPath 'FileManager.exe'
            if (-not (Test-Path -LiteralPath $oldExecutable -PathType Leaf) -or
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $oldExecutable).Hash -ne
                    $ExpectedLowerAppSha256 -or
                    (Test-Path -LiteralPath $defaultInstallPath) -or
                    (Test-Path -LiteralPath $conflictingInstallPath)) {
                throw 'Missing-marker refusal changed or moved the old installation.'
            }
        }
        finally {
            if (-not (Test-Path -LiteralPath $installerRegistryPath)) {
                New-Item -Path $installerRegistryPath -Force | Out-Null
            }
            New-ItemProperty -LiteralPath $installerRegistryPath -Name InstallPath -Value $savedLocation -PropertyType String -Force | Out-Null
        }
        Assert-InstalledLocation 'missing-marker recovery' $ExpectedLowerAppSha256
    }

    $higherInstall = Invoke-Msi '/i' $higherPath 'higher-upgrade.log' $conflictingRequest
    if ($higherInstall.ExitCode -ne 0) {
        throw "Major upgrade failed with exit code $($higherInstall.ExitCode)."
    }
    if ((Get-ProductState $higherProductCode) -ne 5 `
            -or (Get-ProductState $lowerProductCode) -eq 5) {
        throw 'The major upgrade did not leave exactly the higher product registered.'
    }
    if ($UseCustomInstallFolder) { Assert-InstalledLocation 'higher upgrade' $ExpectedHigherAppSha256 }
    if (-not (Test-Path -LiteralPath $previewRuntimePath)) {
        throw 'The major upgrade incorrectly removed the reusable private preview runtime.'
    }
    $higherProbeExit = Invoke-VerifiedProcess $executable $probeArguments 30
    if ($higherProbeExit -ne 0) {
        throw 'The installed sandbox failed after the major upgrade.'
    }

    $downgrade = Invoke-Msi '/i' $lowerPath 'downgrade-refusal.log'
    $downgradeEvidence = [IO.File]::ReadAllText($downgrade.Log)
    if ($downgrade.ExitCode -ne 1603 `
            -or $downgradeEvidence -notmatch 'WIX_DOWNGRADE_DETECTED' `
            -or $downgradeEvidence -notmatch 'A newer version of File Manager is already installed') {
        throw "Downgrade refusal evidence is invalid; exit code $($downgrade.ExitCode)."
    }
    if ((Get-ProductState $higherProductCode) -ne 5 `
            -or (Get-ProductState $lowerProductCode) -eq 5) {
        throw 'The refused downgrade changed the installed product registration.'
    }
    if ($UseCustomInstallFolder) { Assert-InstalledLocation 'downgrade refusal' $ExpectedHigherAppSha256 }
}
catch {
    $primaryFailure = $_
}
finally {
    if (-not $unsafeToCleanup) {
        foreach ($candidate in @(
                @{ Code = $higherProductCode; Path = $higherPath; Log = 'higher-uninstall.log' },
                @{ Code = $lowerProductCode; Path = $lowerPath; Log = 'lower-uninstall.log' })) {
            if ($unsafeToCleanup) { break }
            try {
                if ((Get-ProductState $candidate.Code) -eq 5) {
                    $cleanup = Invoke-Msi '/x' $candidate.Path $candidate.Log
                    if ($cleanup.ExitCode -ne 0) {
                        $cleanupFailures.Add("Uninstall of $($candidate.Path) failed: $($cleanup.ExitCode).")
                    }
                }
            }
            catch { $cleanupFailures.Add("Uninstall of $($candidate.Path) threw: $_") }
        }
        try {
            if (Test-Path -LiteralPath $previewFixture) {
                [IO.File]::Delete($previewFixture)
            }
        }
        catch { $cleanupFailures.Add("Preview fixture cleanup failed: $_") }
    }
}
if ($unsafeToCleanup) {
    [ordered]@{
        PrimaryFailure = "$primaryFailure"
        TimedOutProcessId = $timedOutPid
        CleanupSkipped = $true
        Logs = $logDirectory
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $logDirectory 'result.json')
    throw "A timed-out process may still be active; inspect PID $timedOutPid before cleanup. See $logDirectory."
}

$residue = [ordered]@{
    LowerProductRegistration = (Get-ProductState $lowerProductCode) -eq 5
    HigherProductRegistration = (Get-ProductState $higherProductCode) -eq 5
    DefaultInstallFolder = Test-Path -LiteralPath $defaultInstallPath
    CustomInstallFolder = Test-Path -LiteralPath $customInstallPath
    ConflictingInstallFolder = Test-Path -LiteralPath $conflictingInstallPath
    ShortcutFolder = Test-Path -LiteralPath $shortcutDirectory
    InstallerRegistry = Test-Path -LiteralPath $installerRegistryPath
    PreviewRuntime = Test-Path -LiteralPath $previewRuntimePath
}
$remaining = @($residue.GetEnumerator() | Where-Object Value | ForEach-Object Key)
$result = [ordered]@{
    LowerPackage = $lowerPath
    HigherPackage = $higherPath
    LowerPackageSha256 = $ExpectedLowerPackageSha256.ToUpperInvariant()
    HigherPackageSha256 = $ExpectedHigherPackageSha256.ToUpperInvariant()
    LowerVersion = $LowerVersion
    HigherVersion = $HigherVersion
    InstallPath = $installPath
    ConflictingRequestedPath = if ($UseCustomInstallFolder) { $conflictingInstallPath } else { $null }
    PrimaryFailure = if ($primaryFailure) { "$primaryFailure" } else { $null }
    CleanupFailures = @($cleanupFailures)
    Residue = $residue
    Logs = $logDirectory
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $logDirectory 'result.json')
if ($primaryFailure -or $cleanupFailures.Count -ne 0 -or $remaining.Count -ne 0) {
    throw "MSI upgrade lifecycle failed or left app-owned state; see $logDirectory."
}
if ($UseCustomInstallFolder) {
    $moveFailure = $null
    $moveCleanupFailures = [Collections.Generic.List[string]]::new()
    try {
        $moveInstall = Invoke-Msi '/i' $higherPath 'higher-reinstall-new-folder.log' $conflictingInstallPath
        if ($moveInstall.ExitCode -ne 0) {
            throw "Deliberate move reinstall failed: $($moveInstall.ExitCode)."
        }
        if ((Get-ProductState $higherProductCode) -ne 5 -or
                (Get-ProductState $lowerProductCode) -eq 5) {
            throw 'Deliberate move did not register exactly the higher product.'
        }
        $movedExe = Join-Path $conflictingInstallPath 'FileManager.exe'
        if (-not (Test-Path -LiteralPath $movedExe -PathType Leaf) -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $movedExe).Hash -ne
                $ExpectedHigherAppSha256 -or
                (Test-Path -LiteralPath $defaultInstallPath) -or
                (Test-Path -LiteralPath $customInstallPath)) {
            throw 'Deliberate move did not place only the expected app in the new folder.'
        }
        $movedMarker = (Get-ItemProperty -LiteralPath $installerRegistryPath -Name InstallPath).InstallPath
        if ([IO.Path]::GetFullPath($movedMarker).TrimEnd('\') -ine
                [IO.Path]::GetFullPath($conflictingInstallPath).TrimEnd('\')) {
            throw 'Deliberate move did not update the installer location marker.'
        }
        if (-not (Test-Path -LiteralPath $shortcutPath -PathType Leaf)) {
            throw 'Deliberate move did not create the Start menu shortcut.'
        }
        $movedTarget = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath).TargetPath
        if ([IO.Path]::GetFullPath($movedTarget) -ine [IO.Path]::GetFullPath($movedExe)) {
            throw 'Deliberate move left the Start menu shortcut at the old location.'
        }
        $moveProbeExit = Invoke-VerifiedProcess $movedExe '--deployment-probe' 30
        if ($moveProbeExit -ne 0) {
            throw "Deliberate move deployment probe failed: $moveProbeExit."
        }
    }
    catch { $moveFailure = $_ }
    finally {
        if (-not $unsafeToCleanup) {
            try {
                if ((Get-ProductState $higherProductCode) -eq 5) {
                    $moveRemoval = Invoke-Msi '/x' $higherPath 'higher-reinstall-uninstall.log'
                    if ($moveRemoval.ExitCode -ne 0) {
                        $moveCleanupFailures.Add("Moved install uninstall failed: $($moveRemoval.ExitCode).")
                    }
                }
            }
            catch { $moveCleanupFailures.Add("Moved install uninstall threw: $_") }
        }
    }
    if ($unsafeToCleanup) {
        $result['MoveAfterUninstall'] = [ordered]@{
            PrimaryFailure = "$moveFailure"
            TimedOutProcessId = $timedOutPid
            CleanupSkipped = $true
        }
        $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $logDirectory 'result.json')
        throw "A timed-out process may still be active; inspect PID $timedOutPid before cleanup. See $logDirectory."
    }
    $moveResidue = [ordered]@{
        LowerProductRegistration = (Get-ProductState $lowerProductCode) -eq 5
        HigherProductRegistration = (Get-ProductState $higherProductCode) -eq 5
        DefaultInstallFolder = Test-Path -LiteralPath $defaultInstallPath
        OriginalInstallFolder = Test-Path -LiteralPath $customInstallPath
        NewInstallFolder = Test-Path -LiteralPath $conflictingInstallPath
        ShortcutFolder = Test-Path -LiteralPath $shortcutDirectory
        InstallerRegistry = Test-Path -LiteralPath $installerRegistryPath
        PreviewRuntime = Test-Path -LiteralPath $previewRuntimePath
    }
    $moveRemaining = @($moveResidue.GetEnumerator() | Where-Object Value | ForEach-Object Key)
    $result['MoveAfterUninstall'] = [ordered]@{
        OriginalFolder = $customInstallPath
        NewFolder = $conflictingInstallPath
        PrimaryFailure = if ($moveFailure) { "$moveFailure" } else { $null }
        CleanupFailures = @($moveCleanupFailures)
        Residue = $moveResidue
    }
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $logDirectory 'result.json')
    if ($moveFailure -or $moveCleanupFailures.Count -ne 0 -or $moveRemaining.Count -ne 0) {
        throw "Deliberate uninstall/reinstall move failed or left app-owned state; see $logDirectory."
    }
}
[pscustomobject]@{
    LowerVersion = $LowerVersion
    HigherVersion = $HigherVersion
    InstallPath = $installPath
    CustomFolderRetention = if ($UseCustomInstallFolder) { 'Passed' } else { 'Not selected' }
    MoveAfterUninstall = if ($UseCustomInstallFolder) { 'Passed' } else { 'Not selected' }
    MajorUpgrade = 'Passed'
    SandboxAfterUpgrade = 'Passed'
    DowngradeRefusal = 'Passed (1603 with WIX_DOWNGRADE_DETECTED)'
    FinalUninstall = 'Passed'
    AppOwnedResidue = 'None'
    Logs = $logDirectory
} | Format-List








