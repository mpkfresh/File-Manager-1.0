[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Package,
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.\d+)?$')]
    [string]$ExpectedVersion = '0.8.7.6',
    [string]$ExpectedProductName = 'File Manager',
    [string]$ExpectedManufacturer = 'Kiatami',
    [switch]$ReviewOnlyPrerelease,
    [switch]$AcceptWixEula,
    [switch]$RequireSignature,
    [ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$ExpectedSignerThumbprint
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReleaseSignatures.ps1')
. (Join-Path $PSScriptRoot 'PayloadInventory.ps1')
$versionContract = Get-FileManagerVersionContract $ExpectedVersion
$installerIdentity = Get-FileManagerInstallerIdentity -ReviewOnlyPrerelease:$ReviewOnlyPrerelease
if ($ReviewOnlyPrerelease -and $ExpectedVersion -cne '1.0.0') {
    throw 'Unsigned preview MSI verification requires version 1.0.0.'
}
if ($ReviewOnlyPrerelease -and ($RequireSignature -or $ExpectedSignerThumbprint)) {
    throw 'Unsigned preview MSI verification cannot request a signing certificate.'
}
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null,
    $installer, @($packagePath, 0))

function Read-MsiRows([string]$Sql, [switch]$MissingTableAllowed) {
    try {
        $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($Sql))
    }
    catch {
        if ($MissingTableAllowed) { return @() }
        throw
    }
    try {
        $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
        $rows = @()
        while ($true) {
            $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
            if ($null -eq $record) { break }
            $count = $record.GetType().InvokeMember('FieldCount', 'GetProperty', $null, $record, $null)
            $values = for ($index = 1; $index -le $count; ++$index) {
                $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @($index))
            }
            $rows += ,([string[]]$values)
        }
        return $rows
    }
    finally {
        $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
    }
}

$properties = @{}
foreach ($row in Read-MsiRows 'SELECT `Property`,`Value` FROM `Property`') {
    $properties[$row[0]] = $row[1]
}
if (($PSBoundParameters.ContainsKey('ExpectedProductName') -and
            $ExpectedProductName -cne $installerIdentity.Name) -or
        $properties.ProductName -cne $installerIdentity.Name -or
        $properties.ProductVersion -ne $versionContract.MsiVersion -or
        $properties.Manufacturer -ne $ExpectedManufacturer) {
    throw 'MSI product identity does not match the requested package identity.'
}
$previousUpgradeCode = '9E0D655D-A984-4D6E-B3A7-76BF9CB47F41'
if (([string]$properties.UpgradeCode -replace '[{}]', '') -ine $installerIdentity.UpgradeCode) {
    throw 'MSI has the wrong installer upgrade family.'
}
$otherChannelProperty = 'FM_OTHER_CHANNEL_PRESENT'
$otherChannelRows = @(Read-MsiRows 'SELECT `UpgradeCode`,`VersionMin`,`VersionMax`,`Attributes`,`ActionProperty` FROM `Upgrade`' |
    Where-Object { $_[4] -ceq $otherChannelProperty })
if ($otherChannelRows.Count -ne 1 -or
        ($otherChannelRows[0][0] -replace '[{}]', '') -ine $installerIdentity.OtherChannelUpgradeCode -or
        $otherChannelRows[0][1] -ne '0.0.0' -or $otherChannelRows[0][2] -ne '' -or
        (([int]$otherChannelRows[0][3]) -band 0x002) -eq 0 -or
        $otherChannelProperty -cnotin @($properties.SecureCustomProperties -split ';')) {
    throw 'MSI must detect the other 1.0 installer family without removing it.'
}
$otherChannelGuards = @(Read-MsiRows 'SELECT `Condition` FROM `LaunchCondition`' |
    Where-Object { $_[0] -ceq 'Installed OR NOT FM_OTHER_CHANNEL_PRESENT' })
if ($otherChannelGuards.Count -ne 1) {
    throw 'MSI must refuse coexistence with the other 1.0 installer family.'
}
$oldSeriesProperty = 'FM_PRE_1_0_PRESENT'
$legacyRows = @(Read-MsiRows 'SELECT `UpgradeCode`,`VersionMin`,`VersionMax`,`Attributes`,`ActionProperty` FROM `Upgrade`' |
    Where-Object { $_[4] -ceq $oldSeriesProperty })
if ($legacyRows.Count -ne 1 -or
        ($legacyRows[0][0] -replace '[{}]', '') -ine $previousUpgradeCode -or
        $legacyRows[0][1] -ne '0.0.0' -or
        $legacyRows[0][2] -ne '1.0.0' -or
        (([int]$legacyRows[0][3]) -band 0x002) -eq 0 -or
        (([int]$legacyRows[0][3]) -band 0x200) -ne 0 -or
        $oldSeriesProperty -cnotin @($properties.SecureCustomProperties -split ';')) {
    throw 'MSI must only detect the earlier 0.x family and preserve it for manual uninstall.'
}
$oldSeriesGuard = 'Installed OR NOT FM_PRE_1_0_PRESENT'
$oldSeriesGuards = @(Read-MsiRows 'SELECT `Condition` FROM `LaunchCondition`' |
    Where-Object { $_[0] -ceq $oldSeriesGuard })
if ($oldSeriesGuards.Count -ne 1) {
    throw 'MSI must refuse 1.0 installation until the earlier 0.x family is uninstalled.'
}
if ($properties.ARPPRODUCTICON -ne 'ProductIcon') {
    throw 'The MSI does not register the shared File Manager icon for Installed apps.'
}
$productIcons = @(Read-MsiRows 'SELECT `Name` FROM `Icon`')
if ($productIcons.Count -ne 1 -or $productIcons[0][0] -ne 'ProductIcon') {
    throw 'The MSI must embed exactly one shared product icon.'
}
if ($properties.ContainsKey('ALLUSERS') -or $properties.ContainsKey('MSIINSTALLPERUSER')) {
    throw 'This package must remain fixed per-user, not per-machine or dual-scope.'
}

$summary = $installer.SummaryInformation($packagePath, 0)
$wordCount = [int]$summary.Property(15)
if (($wordCount -band 8) -eq 0) {
    throw 'The MSI summary does not declare that elevated privileges are unnecessary.'
}
if (($wordCount -band 2) -eq 0) {
    throw 'The MSI payload is not marked as compressed.'
}

$files = @(Read-MsiRows 'SELECT `FileName`,`FileSize`,`Version`,`Component_` FROM `File`')
foreach ($row in $files) {
    $longName = ($row[0] -split '\|')[-1]
    if ($longName -match '(?i)(test|benchmark|\.pdb$|vc_redist)') {
        throw "Forbidden development payload entered the MSI: $longName"
    }
}
$applicationVersions = @{}
$applicationNames = @(Get-FileManagerExecutableNames)
foreach ($row in $files) {
    $name = ($row[0] -split '\|')[-1]
    if ($name.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
        if ($name -cnotin $applicationNames -or $applicationVersions.ContainsKey($name)) {
            throw "The MSI contains an unexpected or duplicate executable: $name"
        }
        $applicationVersions[$name] = $row[2]
    }
}
foreach ($name in $applicationNames) {
    if ($applicationVersions[$name] -ne $versionContract.FileVersion) {
        throw "The packaged '$name' version does not match the MSI version."
    }
}

$registryRows = @(Read-MsiRows 'SELECT `Registry`,`Root`,`Component_`,`Key` FROM `Registry`')
$installerRegistryKey = $installerIdentity.RegistryKey
$locationSearches = @(Read-MsiRows 'SELECT `Property`,`Signature_` FROM `AppSearch`')
foreach ($property in @('INSTALLFOLDER', 'FM_EXISTING_INSTALLFOLDER')) {
    $matches = @($locationSearches | Where-Object { $_[0] -ceq $property })
    if ($matches.Count -ne 1 -or $matches[0][1] -cne 'ExistingInstallFolder') {
        throw 'MSI must recover its existing installation folder before resolving directories.'
    }
}
$location = @(Read-MsiRows 'SELECT `Signature_`,`Root`,`Key`,`Name`,`Type` FROM `RegLocator`' |
    Where-Object { $_[0] -ceq 'ExistingInstallFolder' })
if ($location.Count -ne 1 -or [int]$location[0][1] -ne 1 -or
        $location[0][2] -cne $installerRegistryKey -or $location[0][3] -cne 'InstallPath' -or
        [int]$location[0][4] -ne 18) {
    throw 'MSI installation location search must read only the 64-bit per-user ownership marker.'
}
$locationCondition = 'REMOVE~="ALL" OR (NOT Installed AND NOT WIX_UPGRADE_DETECTED) OR FM_EXISTING_INSTALLFOLDER'
$locationGuards = @(Read-MsiRows 'SELECT `Condition` FROM `LaunchCondition`' |
    Where-Object { $_[0] -ceq $locationCondition })
if ($locationGuards.Count -ne 1) { throw 'MSI must refuse an upgrade/repair with an unknown existing location.' }
foreach ($sequenceTable in @('InstallUISequence', 'InstallExecuteSequence')) {
    $sequence = @{}
    foreach ($row in Read-MsiRows ('SELECT `Action`,`Sequence` FROM `' + $sequenceTable + '`')) {
        $sequence[$row[0]] = [int]$row[1]
    }
    if (-not $sequence.ContainsKey('AppSearch') -or -not $sequence.ContainsKey('LaunchConditions') -or
            -not $sequence.ContainsKey('CostFinalize') -or -not $sequence.ContainsKey('FindRelatedProducts') -or
            $sequence.AppSearch -ge $sequence.LaunchConditions -or
            $sequence.FindRelatedProducts -ge $sequence.LaunchConditions -or
            $sequence.AppSearch -ge $sequence.CostFinalize) {
        throw 'MSI location recovery and upgrade detection must precede launch conditions and directory resolution.'
    }
}
foreach ($row in $registryRows) {
    if ([int]$row[1] -ne 1 -or ($row[3] -cne $installerRegistryKey -and
            $row[3] -cne "$installerRegistryKey\Components" -and
            $row[3] -cne "$installerRegistryKey\Directories")) {
        throw 'MSI registry ownership escaped the stable per-user installer namespace.'
    }
}
$registryById = @{}
$installMarkers = @(Read-MsiRows 'SELECT `Key`,`Name`,`Value` FROM `Registry`' | Where-Object {
    $_[0] -ceq $installerRegistryKey -and $_[1] -ceq 'InstallPath'
})
if ($installMarkers.Count -ne 1 -or $installMarkers[0][2] -cne '[INSTALLFOLDER]') {
    throw 'MSI must register one exact per-user installation path for folder-routing ownership.'
}
foreach ($row in $registryRows) { $registryById[$row[0]] = $row }
$components = @(Read-MsiRows 'SELECT `Component`,`Directory_`,`KeyPath` FROM `Component`')
$startMenuComponent = @(Read-MsiRows 'SELECT `Component`,`ComponentId` FROM `Component`' |
    Where-Object { $_[0] -ceq 'StartMenuShortcut' })
if ($startMenuComponent.Count -ne 1 -or
        ($startMenuComponent[0][1] -replace '[{}]', '') -ine $installerIdentity.StartMenuComponentGuid) {
    throw 'MSI Start menu component does not match its installer family.'
}
$mainFileRows = @($files | Where-Object { ($_[0] -split '\|')[-1] -ceq 'FileManager.exe' })
$componentGuids = @{}
foreach ($row in Read-MsiRows 'SELECT `Component`,`ComponentId` FROM `Component`') {
    $componentGuids[$row[0]] = $row[1] -replace '[{}]', ''
}
if ($mainFileRows.Count -ne 1 -or -not $componentGuids.ContainsKey($mainFileRows[0][3])) {
    throw 'MSI main executable component is missing.'
}
$mainSeed = $installerIdentity.ComponentSeedPrefix + 'file:FileManager.exe'
$mainHash = [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(
    $mainSeed.ToLowerInvariant()))
$mainGuidBytes = [byte[]]::new(16)
[Array]::Copy($mainHash, $mainGuidBytes, 16)
$mainGuidBytes[6] = ($mainGuidBytes[6] -band 0x0f) -bor 0x50
$mainGuidBytes[8] = ($mainGuidBytes[8] -band 0x3f) -bor 0x80
$expectedMainGuid = ([Guid]::new($mainGuidBytes)).ToString().ToUpperInvariant()
if ($componentGuids[$mainFileRows[0][3]] -ine $expectedMainGuid) {
    throw 'MSI executable component does not belong to its installer family.'
}
foreach ($row in $components) {
    if (-not $registryById.ContainsKey($row[2]) -or [int]$registryById[$row[2]][1] -ne 1) {
        throw "Component $($row[0]) does not use an HKCU registry key path."
    }
}

$directoryRows = @(Read-MsiRows 'SELECT `Directory`,`Directory_Parent`,`DefaultDir` FROM `Directory`')
$installFolderRows = @($directoryRows | Where-Object { $_[0] -ceq 'INSTALLFOLDER' })
if ($installFolderRows.Count -ne 1 -or
        (($installFolderRows[0][2] -split ':')[0] -split '\|')[-1] -cne $installerIdentity.InstallFolder) {
    throw 'MSI installation folder does not match its installer family.'
}
$menuFolderRows = @($directoryRows | Where-Object { $_[0] -ceq 'ApplicationProgramsFolder' })
if ($menuFolderRows.Count -ne 1 -or
        (($menuFolderRows[0][2] -split ':')[0] -split '\|')[-1] -cne $installerIdentity.Name) {
    throw 'MSI Start menu folder does not match its installer family.'
}
$payloadDirectories = @{ INSTALLFOLDER = '' }
do {
    $before = $payloadDirectories.Count
    foreach ($row in $directoryRows) {
        if ($payloadDirectories.ContainsKey($row[0]) -or -not $payloadDirectories.ContainsKey($row[1])) { continue }
        # DefaultDir permits target:source and short|long forms. Compare the installed target.
        $leaf = (($row[2] -split ':')[0] -split '\|')[-1]
        if ($leaf -eq '.') { $payloadDirectories[$row[0]] = $payloadDirectories[$row[1]]; continue }
        if ([string]::IsNullOrWhiteSpace($leaf) -or $leaf -match '[\\/:]' -or $leaf -eq '..') {
            throw "Invalid MSI payload directory: $($row[0])"
        }
        $parent = $payloadDirectories[$row[1]]
        $payloadDirectories[$row[0]] = if ($parent) { "$parent/$leaf" } else { $leaf }
    }
} while ($before -ne $payloadDirectories.Count)
$componentDirectories = @{}
foreach ($row in $components) { $componentDirectories[$row[0]] = $row[1] }
$payloadPaths = @(foreach ($row in $files) {
    $directory = $componentDirectories[$row[3]]
    if (-not $directory -or -not $payloadDirectories.ContainsKey($directory)) {
        throw 'MSI payload file is outside INSTALLFOLDER or has an unresolved component.'
    }
    $leaf = ($row[0] -split '\|')[-1]
    if ([string]::IsNullOrWhiteSpace($leaf) -or $leaf -match '[\\/:]' -or $leaf -in @('.', '..')) {
        throw "Invalid MSI payload file name: $leaf"
    }
    $parent = $payloadDirectories[$directory]
    if ($parent) { "$parent/$leaf" } else { $leaf }
})
Assert-FileManagerPayloadPaths -Paths $payloadPaths -ProductVersion $ExpectedVersion
$ownedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
[void]$ownedDirectories.Add('INSTALLFOLDER')
do {
    $before = $ownedDirectories.Count
    foreach ($row in $directoryRows) {
        if ($ownedDirectories.Contains($row[1])) { [void]$ownedDirectories.Add($row[0]) }
    }
} while ($ownedDirectories.Count -ne $before)
$removedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($row in Read-MsiRows 'SELECT `DirProperty` FROM `RemoveFile`') {
    [void]$removedDirectories.Add($row[0])
}
foreach ($directory in $ownedDirectories) {
    if (-not $removedDirectories.Contains($directory)) {
        throw "App-owned MSI directory lacks explicit uninstall cleanup: $directory"
    }
}

$customActions = @(Read-MsiRows 'SELECT `Action`,`Type`,`Source`,`Target` FROM `CustomAction`' `
    -MissingTableAllowed)
if ($customActions.Count -ne 1) {
    throw "Expected one narrowly scoped uninstall action; found $($customActions.Count)."
}
$cleanupAction = $customActions[0]
$cleanupType = [int]$cleanupAction[1]
if ($cleanupAction[0] -ne 'CleanupPreviewSandbox' `
        -or ($cleanupType -band 0x3f) -ne 18 `
        -or ($cleanupType -band 0x400) -eq 0 `
        -or ($cleanupType -band 0x800) -ne 0 `
        -or ($cleanupType -band 0xc0) -ne 0 `
        -or $cleanupAction[2] -ne 'FileManagerMaintenanceExecutable' `
        -or $cleanupAction[3] -ne '--uninstall-cleanup') {
    throw 'The MSI preview cleanup action is not the checked, deferred, user-context executable contract.'
}
$cleanupSequence = @(Read-MsiRows "SELECT `Condition`,`Sequence` FROM `InstallExecuteSequence` WHERE `Action`='CleanupPreviewSandbox'")
$removeFilesSequence = @(Read-MsiRows "SELECT `Sequence` FROM `InstallExecuteSequence` WHERE `Action`='RemoveFiles'")
if ($cleanupSequence.Count -ne 1 -or $removeFilesSequence.Count -ne 1 `
        -or $cleanupSequence[0][0] -ne 'REMOVE~="ALL" AND NOT UPGRADINGPRODUCTCODE' `
        -or [int]$cleanupSequence[0][1] -ge [int]$removeFilesSequence[0][0]) {
    throw 'The MSI preview cleanup action is not restricted to full uninstall before RemoveFiles.'
}
$shortcuts = @(Read-MsiRows 'SELECT `Name`,`Target` FROM `Shortcut`')
if ($shortcuts.Count -ne 1 -or $shortcuts[0][1] -ne '[INSTALLFOLDER]FileManager.exe' -or
        ($shortcuts[0][0] -split '\|')[-1] -cne $installerIdentity.Name) {
    throw 'The MSI Start menu shortcut contract is invalid.'
}

if ($AcceptWixEula) {
    & dotnet tool run wix -- msi validate -acceptEula wix7 -sice ICE64 -sice ICE91 $packagePath
    if ($LASTEXITCODE -ne 0) {
        throw "Windows Installer ICE validation failed with exit code $LASTEXITCODE."
    }
}

$signature = Get-AuthenticodeSignature -LiteralPath $packagePath
if ($ReviewOnlyPrerelease -and $signature.Status -ne 'NotSigned') {
    throw 'Unsigned preview MSI must remain unsigned.'
}
if ($RequireSignature -or $ExpectedSignerThumbprint) {
    Assert-ReleaseFileSignature -Path $packagePath `
        -ExpectedSignerThumbprint $ExpectedSignerThumbprint | Out-Null
}
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $packagePath
[pscustomobject]@{
    Package = $packagePath
    Product = "$($properties.ProductName) $($properties.ProductVersion)"
    Scope = 'Per-user; no elevation required'
    Files = $files.Count
    Components = $components.Count
    OwnedDirectories = $ownedDirectories.Count
    CustomActions = $customActions.Count
    Signature = $signature.Status
    SHA256 = $hash.Hash
    IceValidation = if ($AcceptWixEula) { 'Passed (ICE64/ICE91 documented exclusions)' } else { 'Not run' }
} | Format-List
