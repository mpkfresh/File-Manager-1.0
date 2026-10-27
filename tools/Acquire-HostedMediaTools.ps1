#Requires -Version 7.0
<#
Prepare the exact MSYS2 and Qt ShaderTools inputs for the reviewed media
rebuild on a fresh GitHub-hosted Windows runner. Prepared but unrun. Downloads
and executable-tool installation require a concrete owner-approved attempt.
This script does not build the app, upload an artifact or publish a release.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputRoot,
    [Parameter(Mandatory)][string]$SevenZipExe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
$msysArchiveName = 'msys2-base-x86_64-20260927.sfx.exe'
$msysArchiveHash = 'AD336CCCFDA47758B5E15CDA993FBBA421115CB0B126697DAEF1EE4DFE37209F'
$msysUrl = "https://github.com/msys2/msys2-installer/releases/download/2026-09-27/$msysArchiveName"
$shaderUrl = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6112/qt6_6112_msvc2022_64/qt.qt6.6112.addons.qtshadertools.win64_msvc2022_64/6.11.2-0-202608131017qtshadertools-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z'
$shaderHash = 'D434C320F5788CF54C71E68AB28488CEB2F1BA725E63241530155347A4C5081E'
$shaderBytes = 3293582L
$packages = @(
    [pscustomobject]@{
        Name = 'make-4.4.1-3-x86_64.pkg.tar.zst'
        Sha256 = 'AF0BDBA17F06FE037F0194069ADAA31A8FE45F1A11381501896AEA1FAE37BD5D'
    },
    [pscustomobject]@{
        Name = 'yasm-1.3.0-3-x86_64.pkg.tar.zst'
        Sha256 = 'F16D746E14AC860C4E94DC49569DE2909A8910371CDC5D93900A93DC3BA2635E'
    },
    [pscustomobject]@{
        Name = 'diffutils-3.12-1-x86_64.pkg.tar.zst'
        Sha256 = '7902C8CE3D4DD69A0F5E98DC9D5C83C17B23314BA486169DB57EF6E2835CE3B6'
    }
)
$expectedTools = [ordered]@{
    'bash.exe' = '7B4DC5D6878AACDEC5D09CA9C5D8E2B43B925A6BBF631873A24C692980B792B2'
    'make.exe' = '91B7E155590D59DB22E5EB0A3FFEEC6350149F70F9ED2C242D133E584EB72FEE'
    'yasm.exe' = 'FE19B077569ADD9864527B11021F5B070CAA94A2338D2AB13AFB3385B0DD0843'
    'diff.exe' = 'D64621F766D8910A92CB3D99BD7F23C0B1C736EECE0E19DCCBCCEAD93AA81854'
}
$expectedShaderFiles = [ordered]@{
    'lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake' = '8008ADBABE7F2C2ACB01A51BB8C9FF2D26993ABB6873279F276F3F23717B1623'
    'lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsToolsConfig.cmake' = '6D947E0C7E7FBB19D6E90341E9DD645D7CBF0E1C2D15037E505DC42DDFD71AF0'
    'lib/cmake/Qt6ShaderToolsTools/Qt6ShaderToolsMacros.cmake' = 'F4AA5B6A1F644958FBBC67D3F859035AEF47937D17442FB6FE2277FA3856976E'
    'bin/qsb.exe' = '2CC3023C8A51401ED2952D22C59397E2BFA0919E6E78A33206F48264FBA8DC9E'
    'bin/Qt6ShaderTools.dll' = 'AB77EB985F5EA7651298D32B87A82182331F19D23DE497463E1CB38C78C5F915'
}

function Assert-OrdinaryAncestors([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Tool path crosses a reparse point: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if (-not $parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
}

function Assert-Hash([string]$Path, [string]$Expected) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -cne $Expected) {
        throw "Downloaded or installed tool hash differs: $($item.Name)"
    }
}

function Invoke-Bounded([string]$Phase, [string]$FilePath,
        [string[]]$Arguments, [int]$Seconds) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $FilePath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw "$Phase did not start." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $done = $process.WaitForExit($Seconds * 1000)
        if (-not $done) { $process.Kill($true); $process.WaitForExit() }
        [IO.File]::WriteAllText((Join-Path $root "logs/$Phase.stdout.log"),
            $stdoutTask.GetAwaiter().GetResult())
        [IO.File]::WriteAllText((Join-Path $root "logs/$Phase.stderr.log"),
            $stderrTask.GetAwaiter().GetResult())
        if (-not $done) { throw "$Phase timed out after $Seconds seconds." }
        if ($process.ExitCode -ne 0) {
            throw "$Phase exited $($process.ExitCode); inspect local logs."
        }
    } finally { $process.Dispose() }
}

function Get-PinnedDownload([string]$Url, [string]$Path,
        [string]$Sha256, [long]$Bytes) {
    if (Test-Path -LiteralPath $Path) { throw 'Refusing occupied download path.' }
    Invoke-WebRequest -Uri $Url -OutFile $Path -MaximumRedirection 5 `
        -TimeoutSec 600 -ErrorAction Stop
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Length -le 0 -or ($Bytes -gt 0 -and $item.Length -ne $Bytes)) {
        throw 'Official download size differs.'
    }
    Assert-Hash $Path $Sha256
}

if (-not [IO.Path]::IsPathFullyQualified($OutputRoot) -or
        $root -cnotmatch '^[A-Za-z]:\\[A-Za-z0-9_.\\-]+$' -or
        $root -eq [IO.Path]::GetPathRoot($root) -or
        (Test-Path -LiteralPath $root)) {
    throw 'A fresh, absolute, simple Windows output directory is required.'
}
Assert-OrdinaryAncestors $root
Assert-OrdinaryAncestors $SevenZipExe
if (-not (Test-Path -LiteralPath $SevenZipExe -PathType Leaf)) {
    throw '7-Zip executable is missing.'
}
[IO.Directory]::CreateDirectory($root) | Out-Null
foreach ($relative in @('downloads', 'logs')) {
    [IO.Directory]::CreateDirectory((Join-Path $root $relative)) | Out-Null
}
$downloads = Join-Path $root 'downloads'
$msysArchive = Join-Path $downloads $msysArchiveName
Get-PinnedDownload $msysUrl $msysArchive $msysArchiveHash 43117824L
$signature = Get-AuthenticodeSignature -LiteralPath $msysArchive
if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        -not $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -cne
            'CN=Christoph Reiter, O=Christoph Reiter, L=Graz, C=AT') {
    throw 'MSYS2 base signature or signer differs.'
}
foreach ($package in $packages) {
    Get-PinnedDownload "https://repo.msys2.org/msys/x86_64/$($package.Name)" `
        (Join-Path $downloads $package.Name) $package.Sha256 0
}
$shaderArchive = Join-Path $downloads 'qtshadertools-6.11.2.7z'
Get-PinnedDownload $shaderUrl $shaderArchive $shaderHash $shaderBytes

# The pinned, signed SFX expands only an msys64 tree under the fresh root.
Invoke-Bounded 'msys-extract' $msysArchive @('-y', "-o$root") 180
$msysBin = Join-Path $root 'msys64/usr/bin'
$cygpath = Join-Path $msysBin 'cygpath.exe'
if (-not (Test-Path -LiteralPath $cygpath -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $msysBin 'pacman.exe') -PathType Leaf)) {
    throw 'The signed MSYS2 base did not provide cygpath and pacman.'
}
$rootPosix = @(& $cygpath -u $root)
if ($LASTEXITCODE -ne 0 -or $rootPosix.Count -ne 1 -or
        -not $rootPosix[0].StartsWith('/')) {
    throw 'Cannot map the isolated tool directory into MSYS2.'
}
$pacmanConfig = Join-Path $root 'pacman-local-only.conf'
@'
[options]
Architecture = auto
SigLevel = Required DatabaseOptional
LocalFileSigLevel = Never
'@ | Set-Content -LiteralPath $pacmanConfig -Encoding utf8NoBOM
$packagePaths = @($packages | ForEach-Object {
    "'$($rootPosix[0])/downloads/$($_.Name)'"
})
$pacmanCommand = "pacman --config '$($rootPosix[0])/pacman-local-only.conf' -U --noconfirm --needed " +
    ($packagePaths -join ' ')
$env:MSYSTEM = 'MSYS'
$env:MSYS2_PATH_TYPE = 'strict'
# The signed MSYS2 login profile supplies /usr/bin; without it, pacman is
# absent from Bash's command path on a clean hosted runner.
Invoke-Bounded 'msys-local-packages' (Join-Path $msysBin 'bash.exe') `
    @('-lc', $pacmanCommand) 240
foreach ($entry in $expectedTools.GetEnumerator()) {
    Assert-Hash (Join-Path $msysBin $entry.Key) $entry.Value
}

$shaders = Join-Path $root 'qtshadertools'
[IO.Directory]::CreateDirectory($shaders) | Out-Null
Invoke-Bounded 'shader-extract' $SevenZipExe @('x', $shaderArchive,
    "-o$shaders", '-y') 180
$configs = @(Get-ChildItem -LiteralPath $shaders -Recurse -File `
    -Filter 'Qt6ShaderToolsConfig.cmake')
if ($configs.Count -ne 1) { throw 'Expected one Qt ShaderTools prefix.' }
$shaderPrefix = $configs[0].Directory.Parent.Parent.Parent.FullName
if (-not $shaderPrefix.StartsWith($shaders + '\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'ShaderTools prefix escaped the isolated tool directory.'
}
foreach ($entry in $expectedShaderFiles.GetEnumerator()) {
    Assert-Hash (Join-Path $shaderPrefix $entry.Key.Replace('/', '\')) `
        $entry.Value
}
[ordered]@{
    schema = 'file-manager-hosted-media-tools-v1'
    status = 'acquired-unreviewed'
    msys2ArchiveSha256 = $msysArchiveHash
    msys2Signer = $signature.SignerCertificate.Subject
    installedTools = $expectedTools
    shaderToolsArchiveSha256 = $shaderHash
    shaderToolsFiles = $expectedShaderFiles
    mediaBuilt = $false
    uploaded = $false
} | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $root 'tool-acquisition-result.json') `
        -Encoding utf8NoBOM
Write-Output 'Acquired pinned hosted media tools locally; no build or upload.'
