function Get-FileManagerExecutableNames {
    @('FileManager.exe', 'FileManagerShellHost.exe', 'FileManagerPreviewHost.exe',
        'FileManagerContextMenuHost.exe', 'FileManagerMaintenance.exe')
}

function Assert-FileManagerOrdinaryPath {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$LiteralPath)
    $item = Get-Item -LiteralPath $LiteralPath -Force -ErrorAction Stop
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Fixture path crosses a reparse point: $($item.FullName)"
        }
        $item = if ($item -is [IO.FileInfo]) { $item.Directory } else { $item.Parent }
    }
}

function Get-FileManagerInstallerIdentity {
    [CmdletBinding()]
    param([switch]$ReviewOnlyPrerelease)
    if ($ReviewOnlyPrerelease) {
        return [pscustomobject]@{
            Name = 'File Manager (Unsigned Preview)'
            InstallFolder = 'File Manager Unsigned Preview'
            RegistryKey = 'Software\Kiatami\FileManager\UnsignedPreview\Installer'
            UpgradeCode = 'F256047E-0C93-4C66-9327-281F55AF03AF'
            OtherChannelUpgradeCode = '5429857B-08FB-4F84-B33F-25C1B23692CA'
            StartMenuComponentGuid = '00DD39E0-AC2F-4EBD-94AC-033FAECD38C6'
            ComponentSeedPrefix = 'unsigned-preview:'
        }
    }
    # Preserve these IDs across later Kiatami upgrades.
    return [pscustomobject]@{
        Name = 'File Manager'
        InstallFolder = 'File Manager'
        RegistryKey = 'Software\Kiatami\FileManager\Installer'
        UpgradeCode = '5429857B-08FB-4F84-B33F-25C1B23692CA'
        OtherChannelUpgradeCode = 'F256047E-0C93-4C66-9327-281F55AF03AF'
        StartMenuComponentGuid = '71E7BB8E-42C7-4701-8CD5-1F9361F98687'
        ComponentSeedPrefix = ''
    }
}

function Get-FileManagerInstallerRegistryKey {
    [CmdletBinding()]
    param([switch]$ReviewOnlyPrerelease)
    (Get-FileManagerInstallerIdentity -ReviewOnlyPrerelease:$ReviewOnlyPrerelease).RegistryKey
}

function Get-FileManagerVersionContract {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Version)
    $parsed = $null
    if ($Version -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$' -or
            -not [Version]::TryParse($Version, [ref]$parsed) -or
            $parsed.Major -gt 255 -or $parsed.Minor -gt 255 -or
            $parsed.Build -gt 65535 -or $parsed.Revision -gt 65535) {
        throw "Version '$Version' is outside the supported MSI/file-version contract."
    }
    $revision = [Math]::Max(0, $parsed.Revision)
    [pscustomobject]@{
        ApplicationVersion = $Version
        FileVersion = '{0}.{1}.{2}.{3}' -f $parsed.Major, $parsed.Minor, $parsed.Build, $revision
        MsiVersion = '{0}.{1}.{2}' -f $parsed.Major, $parsed.Minor, $parsed.Build
    }
}

function Assert-FileManagerMajorUpgradeVersions {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$LowerVersion, [Parameter(Mandatory)][string]$HigherVersion)
    $lower = Get-FileManagerVersionContract $LowerVersion
    $higher = Get-FileManagerVersionContract $HigherVersion
    if ([Version]$higher.MsiVersion -le [Version]$lower.MsiVersion) {
        throw 'A major upgrade must increase one of the first three version fields; Windows Installer ignores the revision field.'
    }
}

function Assert-FileManagerPayloadIdentity {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$PayloadDirectory,
        [string]$ExpectedVersion,
        [string]$ExpectedProductName,
        [string]$ExpectedManufacturer
    )
    $identity = $null
    foreach ($name in Get-FileManagerExecutableNames) {
        $file = Get-Item -LiteralPath (Join-Path $PayloadDirectory $name) -ErrorAction Stop
        $info = $file.VersionInfo
        # Release versions use three numeric parts; progress builds may add one numeric roadmap part.
        if ($null -eq $info -or $info.FileVersion -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$' -or
                [string]::IsNullOrWhiteSpace($info.ProductName) -or
                [string]::IsNullOrWhiteSpace($info.CompanyName) -or
                $info.OriginalFilename -cne $name) {
            throw "Incomplete or invalid application identity in '$name'."
        }
        $numericVersion = [Version](Get-FileManagerVersionContract $info.FileVersion).FileVersion
        $parts = @{ Major = $numericVersion.Major; Minor = $numericVersion.Minor;
            Build = $numericVersion.Build; Private = $numericVersion.Revision }
        foreach ($prefix in @('File', 'Product')) {
            foreach ($part in $parts.Keys) {
                $field = $prefix + $part + 'Part'
                $property = $info.PSObject.Properties[$field]
                if ($null -eq $property -or $null -eq $property.Value -or
                        [string]$property.Value -notmatch '^\d+$' -or
                        [long]$property.Value -ne $parts[$part]) {
                    throw "Numeric application identity differs from the displayed version in '$name' ($field)."
                }
            }
        }
        if ($null -eq $identity) {
            $identity = [pscustomobject]@{
                Version = $info.FileVersion
                ProductName = $info.ProductName
                Manufacturer = $info.CompanyName
            }
        }
        if ($info.FileVersion -cne $identity.Version -or
                $info.ProductName -cne $identity.ProductName -or
                $info.CompanyName -cne $identity.Manufacturer) {
            throw "Application identity differs between the GUI and '$name'."
        }
    }
    if (($ExpectedVersion -and $ExpectedVersion -cne $identity.Version) -or
            ($ExpectedProductName -and $ExpectedProductName -cne $identity.ProductName) -or
            ($ExpectedManufacturer -and $ExpectedManufacturer -cne $identity.Manufacturer)) {
        throw 'Requested package identity does not match the compiled application identity.'
    }
    return $identity
}
