$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\packaging\PackageIdentity.ps1')
& (Join-Path $PSScriptRoot 'payload_inventory_tests.ps1')

$script:publisher = 'Kiatami'
$script:changedFile = ''
$script:changedField = ''
$script:changedValue = ''
$script:missingFile = ''
$script:observed = @()
function Get-Item {
    param($LiteralPath, $ErrorAction)
    $name = [IO.Path]::GetFileName($LiteralPath)
    if ($name -eq $script:missingFile) { throw "Missing fixture: $name" }
    $script:observed += $name
    $info = [pscustomobject]@{
        FileVersion = '0.8.7.6'
        ProductName = 'File Manager'
        CompanyName = $script:publisher
        OriginalFilename = $name
        FileMajorPart = 0; FileMinorPart = 8; FileBuildPart = 7; FilePrivatePart = 6
        ProductMajorPart = 0; ProductMinorPart = 8; ProductBuildPart = 7; ProductPrivatePart = 6
    }
    if ($name -eq $script:changedFile) { $info.($script:changedField) = $script:changedValue }
    [pscustomobject]@{ VersionInfo = $info }
}
function Expect-Rejection([scriptblock]$Action, [string]$Message) {
    $actual = 'No exception'
    $rejected = $false
    try { & $Action | Out-Null } catch {
        $actual = $_.Exception.Message
        $rejected = $actual -like $Message
    }
    if (-not $rejected) { throw "Expected '$Message'; observed: $actual" }
}

$payload = Join-Path $PSScriptRoot 'identity-fixture'
$expectedNames = @('FileManager.exe', 'FileManagerShellHost.exe', 'FileManagerPreviewHost.exe',
    'FileManagerContextMenuHost.exe', 'FileManagerMaintenance.exe')
foreach ($publisher in @('Kiatami', 'Example & Company')) {
    $script:publisher = $publisher
    $script:observed = @()
    $identity = Assert-FileManagerPayloadIdentity $payload -ExpectedVersion '0.8.7.6' `
        -ExpectedProductName 'File Manager' -ExpectedManufacturer $publisher
    if ($identity.Version -cne '0.8.7.6' -or $identity.ProductName -cne 'File Manager' -or
            $identity.Manufacturer -cne $publisher -or $script:observed.Count -ne 5 -or
            (Compare-Object $expectedNames $script:observed)) {
        throw 'The build identity must cover exactly the application and four helpers.'
    }
    if ((Get-FileManagerInstallerRegistryKey) -cne 'Software\Kiatami\FileManager\Installer') {
        throw 'Kiatami releases must retain the 1.0 installer ownership key.'
    }
}
foreach ($name in $expectedNames) {
    $script:missingFile = $name
    Expect-Rejection { Assert-FileManagerPayloadIdentity $payload } '*Missing fixture*'
    $script:missingFile = ''
    $script:changedFile = $name
    foreach ($field in @('FileVersion', 'ProductName', 'CompanyName', 'OriginalFilename')) {
        $script:changedField = $field
        $script:changedValue = ''
        Expect-Rejection { Assert-FileManagerPayloadIdentity $payload } '*invalid application identity*'
        $script:changedValue = if ($field -eq 'FileVersion') { '0.9.0' } else { 'Different' }
        $errorPattern = if ($field -eq 'OriginalFilename') {
            '*invalid application identity*'
        } else { '*identity differs*' }
        Expect-Rejection { Assert-FileManagerPayloadIdentity $payload } $errorPattern
    }
    foreach ($numericField in @('FileMajorPart','FileMinorPart','FileBuildPart','FilePrivatePart',
            'ProductMajorPart','ProductMinorPart','ProductBuildPart','ProductPrivatePart')) {
        $script:changedField = $numericField
        $script:changedValue = 99
        Expect-Rejection { Assert-FileManagerPayloadIdentity $payload } '*Numeric application identity differs*'
        $script:changedValue = $null
        Expect-Rejection { Assert-FileManagerPayloadIdentity $payload } '*Numeric application identity differs*'
    }
    # Regression: text 0.8.7.6 with a hard-coded numeric revision of zero.
    $script:changedField = 'FilePrivatePart'
    $script:changedValue = 0
    Expect-Rejection { Assert-FileManagerPayloadIdentity $payload } '*Numeric application identity differs*'
    $script:changedFile = ''
}
Expect-Rejection {
    Assert-FileManagerPayloadIdentity $payload -ExpectedVersion '0.9.0'
} '*Requested package identity*'
Expect-Rejection {
    Assert-FileManagerPayloadIdentity $payload -ExpectedProductName 'Another Product'
} '*Requested package identity*'
Expect-Rejection {
    Assert-FileManagerPayloadIdentity $payload -ExpectedManufacturer 'Another Publisher'
} '*Requested package identity*'
$script:changedFile = 'FileManager.exe'
$script:changedField = 'FileVersion'
foreach ($invalidVersion in @('0.8', '0.8.6.0.1', '0.8.6-beta')) {
    $script:changedValue = $invalidVersion
    Expect-Rejection { Assert-FileManagerPayloadIdentity $payload } '*invalid application identity*'
}
foreach ($fixture in @(
        @{ Input='0.8.7.6'; Msi='0.8.7'; File='0.8.7.6' },
        @{ Input='1.0.0'; Msi='1.0.0'; File='1.0.0.0' },
        @{ Input='255.255.65535.65535'; Msi='255.255.65535'; File='255.255.65535.65535' })) {
    $contract = Get-FileManagerVersionContract $fixture.Input
    if ($contract.MsiVersion -cne $fixture.Msi -or $contract.FileVersion -cne $fixture.File) {
        throw 'MSI and executable version fields must follow their distinct Windows contracts.'
    }
}
foreach ($invalid in @('1.0', '1.0.0.0.0', '1.0.0-beta', '256.0.0', '1.256.0', '1.0.65536', '1.0.0.65536')) {
    Expect-Rejection { Get-FileManagerVersionContract $invalid } '*outside the supported MSI/file-version contract*'
}
Assert-FileManagerMajorUpgradeVersions '0.8.7.6' '0.8.8.0'
foreach ($higher in @('0.8.7.6', '0.8.7.7', '0.8.6.99')) {
    Expect-Rejection { Assert-FileManagerMajorUpgradeVersions '0.8.7.6' $higher } '*first three version fields*'
}
Write-Output 'Package identity tests passed (five binaries, publisher/ownership, MSI/file versions and upgrade ordering).'
