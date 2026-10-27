# Shared exact path contract for staged, portable and installed MSI payloads.
function Get-FileManagerPayloadPaths {
    param([string]$ProductVersion = '0.9.5')
    $contract = if ([Version]$ProductVersion -ge [Version]'1.0.0') {
        'release-payload-1.0.txt'
    } else { 'release-payload.txt' }
    @(Get-Content -LiteralPath (Join-Path $PSScriptRoot $contract) | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and -not $_.StartsWith('#')
    })
}

function Get-FileManagerRuntimeNames {
    param([string]$ProductVersion = '0.9.5')
    @(Get-FileManagerPayloadPaths -ProductVersion $ProductVersion | Where-Object {
        $_ -match '^(concrt140|msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?|vccorlib140|vcruntime140(_1|_threads)?)\.dll$'
    })
}

function Assert-FileManagerPayloadPaths {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Paths,
        [string]$ProductVersion = '0.9.5')
    $expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in Get-FileManagerPayloadPaths -ProductVersion $ProductVersion) {
        if (-not $expected.Add($path)) { throw "Duplicate release inventory entry: $path" }
    }
    $actual = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $Paths) {
        $relative = $path.Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($relative) -or $relative.StartsWith('/') -or
                $relative -match '[:\x00]|(^|/)\.{1,2}(/|$)|//|/$') {
            throw "Invalid release payload path: $path"
        }
        if (-not $actual.Add($relative)) { throw "Duplicate release payload path: $path" }
    }
    $missing = @($expected | Where-Object { -not $actual.Contains($_) } | Sort-Object)
    $unexpected = @($actual | Where-Object { -not $expected.Contains($_) } | Sort-Object)
    if ($missing.Count -or $unexpected.Count) {
        throw "Release payload inventory differs. Missing: $($missing -join ', '); unexpected: $($unexpected -join ', ')"
    }
}

function Assert-FileManagerPayloadDirectory {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Directory,
        [string]$ProductVersion = '0.9.5')
    $rootItem = Get-Item -LiteralPath $Directory -Force
    if ($rootItem -isnot [IO.DirectoryInfo] -or
            ($rootItem.Attributes -band ([IO.FileAttributes]::ReparsePoint -bor
                [IO.FileAttributes]::Hidden)) -ne 0) {
        throw "Expected an ordinary release payload directory: $Directory"
    }
    $root = $rootItem.FullName.TrimEnd('\', '/')
    $pending = [Collections.Generic.Queue[IO.DirectoryInfo]]::new()
    $pending.Enqueue([IO.DirectoryInfo]::new($root))
    $paths = [Collections.Generic.List[string]]::new()
    while ($pending.Count -gt 0) {
        $parent = $pending.Dequeue()
        foreach ($item in Get-ChildItem -LiteralPath $parent.FullName -Force) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Release payload contains a reparse-backed item: $($item.FullName)"
            }
            if (($item.Attributes -band [IO.FileAttributes]::Hidden) -ne 0) {
                throw "Release payload contains a hidden item that Compress-Archive would omit: $($item.FullName)"
            }
            if ($item -is [IO.DirectoryInfo]) {
                $pending.Enqueue($item)
            } elseif ($item -is [IO.FileInfo]) {
                $paths.Add($item.FullName.Substring($root.Length + 1))
            } else {
                throw "Unexpected release payload item: $($item.FullName)"
            }
        }
    }
    Assert-FileManagerPayloadPaths -Paths ($paths.ToArray()) `
        -ProductVersion $ProductVersion
}
