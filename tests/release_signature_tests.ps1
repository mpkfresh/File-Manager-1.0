$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\packaging\ReleaseSignatures.ps1')

# Stub only OS trust/file observations; exercise the production decision functions.
$script:missing = ''
$script:badFile = ''
$script:status = 'Valid'
$script:signer = 'A' * 40
$script:noCertificate = $false
$script:observed = @()
function Test-Path { param($LiteralPath, $PathType) return $LiteralPath -ne $script:missing }
function Get-AuthenticodeSignature {
    param($LiteralPath)
    $script:observed += $LiteralPath
    [pscustomobject]@{
        Status = if ($LiteralPath -eq $script:badFile) { $script:status } else { 'Valid' }
        SignerCertificate = if ($script:noCertificate) { $null } else {
            [pscustomobject]@{ Thumbprint = $script:signer }
        }
    }
}
function Expect-Rejection([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    $actual = 'No exception'
    try { & $Action | Out-Null } catch {
        $actual = $_.Exception.Message
        $rejected = $actual -like $Message
    }
    if (-not $rejected) { throw "Expected rejection matching '$Message'; observed: $actual" }
}

$payload = Join-Path $PSScriptRoot 'signature-fixture'
$names = @('FileManager.exe', 'FileManagerShellHost.exe', 'FileManagerPreviewHost.exe',
    'FileManagerContextMenuHost.exe', 'FileManagerMaintenance.exe')
$valid = @(Assert-FileManagerBinarySignatures -PayloadDirectory $payload `
    -ExpectedSignerThumbprint ('a' * 40))
if ($valid.Count -ne 5 -or $script:observed.Count -ne 5) {
    throw 'Every app-owned executable must be checked exactly once.'
}
foreach ($name in $names) {
    $path = Join-Path $payload $name
    if ($path -notin $script:observed) { throw "Unchecked executable: $name" }
    $script:badFile = ''
    $script:missing = $path
    Expect-Rejection { Assert-FileManagerBinarySignatures $payload } '*file is missing*'
    $script:missing = ''
    $script:badFile = $path
    foreach ($state in @('NotSigned', 'HashMismatch', 'NotTrusted', 'UnknownError',
            'NotSupportedFileFormat', 'Incompatible', 'UnexpectedStatus')) {
        $script:status = $state
        Expect-Rejection { Assert-FileManagerBinarySignatures $payload } '*signature rejected*'
    }
}
$script:badFile = ''
Expect-Rejection {
    Assert-FileManagerBinarySignatures $payload -ExpectedSignerThumbprint ('B' * 40)
} '*signer does not match*'
$script:noCertificate = $true
Expect-Rejection { Assert-FileManagerBinarySignatures $payload } '*signature rejected*'
$script:noCertificate = $false
Assert-ReleaseFileSignature -Path 'fixture.msi' -ExpectedSignerThumbprint ('A' * 40) | Out-Null
$script:badFile = 'fixture.msi'
$script:status = 'HashMismatch'
Expect-Rejection { Assert-ReleaseFileSignature 'fixture.msi' } '*signature rejected*'
Write-Output 'Release signature policy tests passed (all five executables and MSI).'
