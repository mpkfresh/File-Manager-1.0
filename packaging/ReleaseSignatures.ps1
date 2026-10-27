# Read-only release checks. Signing and certificate selection are deliberately external.
. (Join-Path $PSScriptRoot 'PackageIdentity.ps1')
function Assert-ReleaseFileSignature {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path,
        [string]$ExpectedSignerThumbprint
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required signed release file is missing: $Path"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate) {
        throw "Release signature rejected for '$Path': $($signature.Status)."
    }
    if ($ExpectedSignerThumbprint -and
            $signature.SignerCertificate.Thumbprint -ine $ExpectedSignerThumbprint) {
        throw "Release signer does not match the approved certificate for '$Path'."
    }
    [pscustomobject]@{
        Path = $Path
        Status = $signature.Status
        SignerThumbprint = $signature.SignerCertificate.Thumbprint
    }
}

function Assert-FileManagerBinarySignatures {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$PayloadDirectory,
        [string]$ExpectedSignerThumbprint
    )

    foreach ($name in Get-FileManagerExecutableNames) {
        Assert-ReleaseFileSignature -Path (Join-Path $PayloadDirectory $name) `
            -ExpectedSignerThumbprint $ExpectedSignerThumbprint
    }
}
