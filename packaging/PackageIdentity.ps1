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

function Get-FileManagerInstallerRegistryKey {
    # 1.0 installer ownership ID. Preserve across later Kiatami upgrades.
    'Software\Kiatami\FileManager\Installer'
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
