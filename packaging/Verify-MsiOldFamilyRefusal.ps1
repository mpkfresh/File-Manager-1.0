[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$OldPackage,
    [Parameter(Mandatory)] [string]$NewPackage,
    [Parameter(Mandatory)] [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedOldPackageSha256,
    [Parameter(Mandatory)] [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedNewPackageSha256,
    [Parameter(Mandatory)] [string]$OldDataRoot,
    [Parameter(Mandatory)] [string]$OldDataSentinel,
    [Parameter(Mandatory)] [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedSentinelSha256,
    [Parameter(Mandatory)] [string]$EvidenceRoot,
    [Parameter(Mandatory)] [string]$ExpectedComputerName,
    [Parameter(Mandatory)] [string]$ExpectedCurrentUserSid
)

# V4/L01 only: the older package must already be installed in a disposable
# standard-user profile, with a synthetic sentinel already in its data folder.
# This check attempts the newer installation once and never uninstalls either
# product. The separately observed old-version uninstall follows afterward.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT' -or $env:COMPUTERNAME -ine $ExpectedComputerName -or
        [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ine
            $ExpectedCurrentUserSid) {
    throw 'The disposable Windows machine or user does not match the approved fixture.'
}

$oldPath = (Resolve-Path -LiteralPath $OldPackage).Path
$newPath = (Resolve-Path -LiteralPath $NewPackage).Path
$oldDataPath = (Resolve-Path -LiteralPath $OldDataRoot).Path.TrimEnd('\')
$sentinelPath = (Resolve-Path -LiteralPath $OldDataSentinel).Path
$evidenceRootPath = (Resolve-Path -LiteralPath $EvidenceRoot).Path
. (Join-Path $PSScriptRoot 'PackageIdentity.ps1')
foreach ($fixturePath in @($oldPath, $newPath, $oldDataPath, $sentinelPath,
        $evidenceRootPath)) {
    Assert-FileManagerOrdinaryPath -LiteralPath $fixturePath
}
$evidenceRootItem = Get-Item -LiteralPath $evidenceRootPath -Force
if ($evidenceRootItem -isnot [IO.DirectoryInfo]) {
    throw 'The named evidence root must be an existing directory.'
}
$localAppData = [IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\')
if (-not $oldDataPath.StartsWith(
        $localAppData + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The old-data root must be inside this user profile.'
}
if ($evidenceRootPath.Equals($oldDataPath, [StringComparison]::OrdinalIgnoreCase) -or
        $evidenceRootPath.StartsWith(
            $oldDataPath + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Keep MSI evidence outside the old user-data folder.'
}
$oldDataItem = Get-Item -LiteralPath $oldDataPath -Force
if ($oldDataItem -isnot [IO.DirectoryInfo] -or
        -not $sentinelPath.StartsWith(
            $oldDataPath + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The synthetic sentinel must be inside the chosen old-data folder.'
}
$fixtures = @(
    [pscustomobject]@{ Path = $oldPath; Hash = $ExpectedOldPackageSha256 }
    [pscustomobject]@{ Path = $newPath; Hash = $ExpectedNewPackageSha256 }
    [pscustomobject]@{ Path = $sentinelPath; Hash = $ExpectedSentinelSha256 }
)
foreach ($fixture in $fixtures) {
    $item = Get-Item -LiteralPath $fixture.Path -Force
    if ($item -isnot [IO.FileInfo] -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-FileHash -LiteralPath $fixture.Path -Algorithm SHA256).Hash -ine
                $fixture.Hash) {
        throw "Fixture file is missing, reparse-backed or changed: $($fixture.Path)"
    }
}
if (-not ('FileManagerTransitionMsiNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class FileManagerTransitionMsiNative {
    [DllImport("msi.dll", CharSet = CharSet.Unicode)]
    public static extern int MsiQueryProductState(string productCode);
}
'@
}

function Read-MsiProperty([string]$PackagePath, [string]$Name) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod',
        $null, $installer, @($PackagePath, 0))
    $q = [char]96
    $sql = "SELECT ${q}Value${q} FROM ${q}Property${q} WHERE ${q}Property${q}='$Name'"
    $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null,
        $database, @($sql))
    try {
        $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view,
            $null) | Out-Null
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null,
            $view, $null)
        if ($null -eq $record) { throw "MSI property is missing: $Name" }
        return [string]$record.GetType().InvokeMember('StringData', 'GetProperty',
            $null, $record, @(1))
    }
    finally {
        $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view,
            $null) | Out-Null
    }
}

$oldCode = Read-MsiProperty $oldPath 'ProductCode'
$newCode = Read-MsiProperty $newPath 'ProductCode'
$oldFamily = (Read-MsiProperty $oldPath 'UpgradeCode') -replace '[{}]', ''
$newFamily = (Read-MsiProperty $newPath 'UpgradeCode') -replace '[{}]', ''
$oldVersion = [Version](Read-MsiProperty $oldPath 'ProductVersion')
$newVersion = [Version](Read-MsiProperty $newPath 'ProductVersion')
if ($oldFamily -ine '9E0D655D-A984-4D6E-B3A7-76BF9CB47F41' -or
        $newFamily -ine '5429857B-08FB-4F84-B33F-25C1B23692CA' -or
        $oldVersion -ge [Version]'1.0.0' -or $newVersion -lt [Version]'1.0.0' -or
        $oldCode -ieq $newCode -or
        (Read-MsiProperty $oldPath 'ProductName') -cne 'File Manager' -or
        (Read-MsiProperty $newPath 'ProductName') -cne 'File Manager' -or
        (Read-MsiProperty $newPath 'Manufacturer') -cne 'Kiatami') {
    throw 'The MSI pair is not the approved old-family-to-Kiatami transition.'
}
if ([FileManagerTransitionMsiNative]::MsiQueryProductState($oldCode) -ne 5 -or
        [FileManagerTransitionMsiNative]::MsiQueryProductState($newCode) -eq 5) {
    throw 'The old MSI must be installed and the new MSI absent before this check.'
}

$evidence = Join-Path $evidenceRootPath ('FileManagerOldFamilyRefusal-' +
    (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 8))
[IO.Directory]::CreateDirectory($evidence) | Out-Null
$log = Join-Path $evidence 'new-install-refusal.log'
$process = [Diagnostics.Process]::new()
$process.StartInfo.FileName = 'msiexec.exe'
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.CreateNoWindow = $true
$process.StartInfo.Arguments = '/i "' + $newPath +
    '" /qn /norestart /l*v "' + $log + '"'
try {
    if (-not $process.Start()) { throw 'Could not start Windows Installer.' }
    if (-not $process.WaitForExit(180000)) {
        throw "Windows Installer exceeded 180 seconds; leave the disposable VM intact for inspection (PID $($process.Id))."
    }
    $installExit = $process.ExitCode
    $logText = [IO.File]::ReadAllText($log)
    if ($installExit -ne 1603 -or
            $logText -notmatch 'Uninstall File Manager 0\.x' -or
            [FileManagerTransitionMsiNative]::MsiQueryProductState($oldCode) -ne 5 -or
            [FileManagerTransitionMsiNative]::MsiQueryProductState($newCode) -eq 5 -or
            (Get-FileHash -LiteralPath $sentinelPath -Algorithm SHA256).Hash -ine
                $ExpectedSentinelSha256) {
        throw 'The new MSI did not refuse the old installation while preserving its state.'
    }
    [ordered]@{ oldMsiSha256 = $ExpectedOldPackageSha256;
        newMsiSha256 = $ExpectedNewPackageSha256;
        sentinelSha256 = $ExpectedSentinelSha256;
        oldVersion = $oldVersion.ToString(); newVersion = $newVersion.ToString();
        installExitCode = $installExit; oldProductStillInstalled = $true;
        newProductAbsent = $true; oldDataUnchanged = $true } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'result.json')
    Write-Output "Old-family refusal passed. Evidence: $evidence"
} catch {
    [IO.File]::WriteAllText((Join-Path $evidence 'failure.txt'), $_.ToString())
    throw
} finally {
    $process.Dispose()
}
