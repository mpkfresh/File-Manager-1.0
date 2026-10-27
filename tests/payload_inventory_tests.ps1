$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\packaging\PayloadInventory.ps1')

function Expect-InventoryRejection([string[]]$Paths, [string]$Pattern) {
    $rejected = $false
    try { Assert-FileManagerPayloadPaths -Paths $Paths } catch {
        $rejected = $_.Exception.Message -like $Pattern
        if (-not $rejected) { throw }
    }
    if (-not $rejected) { throw "Expected inventory rejection: $Pattern" }
}

$approved = @(Get-FileManagerPayloadPaths)
Assert-FileManagerPayloadPaths -Paths $approved
Assert-FileManagerPayloadPaths -Paths @($approved | ForEach-Object { $_.Replace('/', '\').ToUpperInvariant() })
$oneOh = @(Get-FileManagerPayloadPaths -ProductVersion '1.0.0')
Assert-FileManagerPayloadPaths -Paths $oneOh -ProductVersion '1.0.0'
if ($oneOh.Count -ne $approved.Count -or
        'manifest/media-provenance.json' -cnotin $oneOh -or
        'sbom/qtmultimedia-6.11.2.cdx.json' -cin $oneOh) {
    throw 'The 1.0 payload must replace only the vendor Multimedia SBOM with the reviewed media record.'
}
$oldContractAccepted = $true
try { Assert-FileManagerPayloadPaths -Paths $approved -ProductVersion '1.0.0' } catch {
    $oldContractAccepted = $false
}
if ($oldContractAccepted) { throw 'The 1.0 payload accepted the stale vendor Multimedia SBOM.' }

# Keep the count unchanged while replacing a required item: a count-only gate missed this.
$withoutLicense = @($approved | Where-Object { $_ -cne 'LICENSE' })
if ($withoutLicense.Count -ne $approved.Count - 1) { throw 'The root license must be an explicit inventory entry.' }
Expect-InventoryRejection ($withoutLicense + 'unexpected.dll') '*Missing: LICENSE; unexpected: unexpected.dll*'
$misplacedPlugin = @($approved | ForEach-Object {
    if ($_ -eq 'platforms/qwindows.dll') { 'qwindows.dll' } else { $_ }
})
Expect-InventoryRejection $misplacedPlugin '*Missing: platforms/qwindows.dll; unexpected: qwindows.dll*'
Expect-InventoryRejection ($approved + 'FileManagerFolderRoutingProbe.exe') '*unexpected: FileManagerFolderRoutingProbe.exe*'
Expect-InventoryRejection ($approved + 'filemanager.exe') '*Duplicate release payload path*'
Expect-InventoryRejection @() '*Release payload inventory differs*'
foreach ($invalid in @('../FileManager.exe', '/FileManager.exe', 'C:/FileManager.exe',
        'platforms//qwindows.dll', './LICENSE', 'LICENSE/', 'platforms/../LICENSE')) {
    Expect-InventoryRejection @($invalid) '*Invalid release payload path*'
}
Write-Output 'Payload inventory tests passed (missing/replaced files, plugin paths, extra executables, duplicates and traversal).'
