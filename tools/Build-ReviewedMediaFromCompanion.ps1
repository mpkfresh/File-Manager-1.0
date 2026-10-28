#Requires -Version 7.0
<#
Build the proposed 1.0 seven-file media set from a hash-pinned source
companion in fresh directories. This source is prepared but unrun. Execution
requires the owner's direct approval for the exact host and attempt.

The caller supplies the reviewed companion, an isolated MSYS2 tool directory,
the already hash-pinned Qt 6.11.2 MSVC kit, ShaderTools and Vulkan SDK build
inputs, and a VS2022 developer environment. This script makes no network
request, changes no installed Qt kit, and does not mark any result
release-approved.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CompanionDirectory,
    [Parameter(Mandatory)][string]$OutputRoot,
    [Parameter(Mandatory)][string]$MsysBin,
    [Parameter(Mandatory)][string]$QtPrefix,
    [Parameter(Mandatory)][string]$ShaderToolsPrefix,
    [Parameter(Mandatory)][string]$VulkanSdkPrefix,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$VulkanHeaderSha256,
    [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$VulkanImportLibrarySha256,
    [Parameter(Mandatory)][string]$VcVars64,
    [Parameter(Mandatory)][string]$CMakeExe,
    [Parameter(Mandatory)][string]$NinjaExe,
    [Parameter(Mandatory)][string]$PythonExe,
    [Parameter(Mandatory)][string]$GitExe,
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$ExpectedVcToolsVersion = '14.44.35207'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\')
$companion = [IO.Path]::GetFullPath($CompanionDirectory).TrimEnd('\')
$extractor = Join-Path $PSScriptRoot 'Extract-PinnedMediaArchive.py'
$checksumListHash = '5123D85407AD0E077A627945E22F220F4D2778DEE1459C610ECB736BA7F66D13'
$archiveHashes = [ordered]@{
    'source/ffmpeg-n7.1.5.tar.gz' = 'E3963A50831C985933E1A625ED566EC4C7ADB5C012C34FA9F84438E1D61BDACC'
    'source/qtmultimedia-everywhere-src-6.11.2.tar.xz' = '967B5E02EC6B793CDB360622CD6E703132836AF983208D678DAE4B50F109CD9F'
    'source/zlib-1.3.1.tar.gz' = '9A93B2B7DFDAC77CEBA5A558A580E74667DD6FEDE4585B91EEFB60F03B72DF23'
}
$toolHashes = [ordered]@{
    'bash.exe' = '7B4DC5D6878AACDEC5D09CA9C5D8E2B43B925A6BBF631873A24C692980B792B2'
    'make.exe' = '91B7E155590D59DB22E5EB0A3FFEEC6350149F70F9ED2C242D133E584EB72FEE'
    'yasm.exe' = 'FE19B077569ADD9864527B11021F5B070CAA94A2338D2AB13AFB3385B0DD0843'
    'diff.exe' = 'D64621F766D8910A92CB3D99BD7F23C0B1C736EECE0E19DCCBCCEAD93AA81854'
}
$ffmpegNames = @('avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll',
    'swresample-5.dll', 'swscale-8.dll')
$sourceFiles = @(
    'build/media-source-and-relink.md',
    'licenses/FFmpeg-LGPL-2.1.txt',
    'licenses/Qt-ThirdParty-Attributions.txt',
    'licenses/Signalsmith-MIT.txt',
    'patches/qt-ffmpeg-attribution.patch',
    'patches/zconf.h.patch',
    'README.md',
    'source/ffmpeg-n7.1.5.tar.gz',
    'source/qtmultimedia-everywhere-src-6.11.2.tar.xz',
    'source/zlib-1.3.1.tar.gz',
    'THIRD_PARTY_NOTICES.md'
)

function Assert-OrdinaryAncestors([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Input or output crosses a reparse point: $cursor"
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
        throw "Missing, reparse-backed or changed file: $Path"
    }
}

function Write-QtBuildDiagnosticSummary([string]$Stdout, [string]$Stderr) {
    # GitHub logs are public. Emit only compiler codes and short messages,
    # without source paths, URLs, email addresses or credential-like text.
    $combined = $Stdout + "`n" + $Stderr
    $diagnostics = [regex]::Matches($combined,
        '(?im)\b(?:fatal )?error\s+(?<code>(?:C|D|LNK|MSB)\d{4,5}):\s*(?<detail>[^\r\n]{0,300})')
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    $emitted = 0
    foreach ($match in $diagnostics) {
        $code = $match.Groups['code'].Value.ToUpperInvariant()
        $detail = $match.Groups['detail'].Value
        $detail = [regex]::Replace($detail, '(?i)https?://\S+', '<url>')
        $detail = [regex]::Replace($detail,
            '(?i)\b[A-Z]:[\\/][^''"]+', '<path>')
        $detail = [regex]::Replace($detail,
            '(?i)(?:\\\\|//)[^''"]+', '<path>')
        $detail = [regex]::Replace($detail,
            '(?i)\b[\w.+-]+@[\w.-]+\.[A-Z]{2,}\b', '<email>')
        if ($detail -match '(?i)(token|password|secret|bearer|authorization|api.?key)') {
            $detail = '<redacted>'
        }
        $detail = [regex]::Replace($detail, '[^\x20-\x7E]', '?').Trim()
        if ($detail.Length -gt 180) { $detail = $detail.Substring(0, 180) }
        $summary = "${code}: $detail"
        if ($seen.Add($summary)) {
            Write-Output "Qt compiler diagnostic: $summary"
            $emitted++
            if ($emitted -ge 12) { break }
        }
    }
    if ($emitted -eq 0) {
        Write-Output 'Qt build failed without a recognized MSVC diagnostic code; raw logs remain runner-local.'
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
        $completed = $process.WaitForExit($Seconds * 1000)
        if (-not $completed) { $process.Kill($true); $process.WaitForExit() }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $root "logs/$Phase.stdout.log"),
            $stdout)
        [IO.File]::WriteAllText((Join-Path $root "logs/$Phase.stderr.log"),
            $stderr)
        if (-not $completed) { throw "$Phase exceeded $Seconds seconds." }
        if ($process.ExitCode -ne 0) {
            if ($Phase -ceq 'qt-build') {
                Write-QtBuildDiagnosticSummary $stdout $stderr
            }
            throw "$Phase exited $($process.ExitCode); inspect local logs."
        }
    } finally {
        $process.Dispose()
    }
}

function Invoke-CmdFile([string]$Phase, [string]$CommandFile, [int]$Seconds) {
    Invoke-Bounded $Phase (Join-Path $env:SystemRoot 'System32/cmd.exe') `
        @('/d', '/c', $CommandFile) $Seconds
}

function Convert-ToMsysPath([string]$Path) {
    $converted = @(& (Join-Path $MsysBin 'cygpath.exe') -u $Path)
    if ($LASTEXITCODE -ne 0 -or $converted.Count -ne 1 -or
            -not $converted[0].StartsWith('/')) {
        throw 'MSYS2 could not map an isolated build path.'
    }
    return $converted[0]
}

# The Bash build recipe uses simple single-quoted paths. Reject ambiguous
# output names before creating any directory or launching any process.
if (-not [IO.Path]::IsPathFullyQualified($OutputRoot) -or
        $root -cnotmatch '^[A-Za-z]:\\[A-Za-z0-9_.\\-]+$' -or
        $root -eq [IO.Path]::GetPathRoot($root) -or
        (Test-Path -LiteralPath $root)) {
    throw 'A fresh, absolute, space-free output directory is required.'
}
foreach ($commandPath in @($MsysBin, $QtPrefix, $ShaderToolsPrefix,
        $VulkanSdkPrefix,
        $VcVars64, $CMakeExe, $NinjaExe)) {
    if ($commandPath.Contains('"') -or
            $commandPath -match '[^\x20-\x7E]|[&|<>^%!]') {
        throw 'A build input path has an unsafe cmd.exe character.'
    }
}
foreach ($path in @($root, $companion, $MsysBin, $QtPrefix,
        $ShaderToolsPrefix, $VulkanSdkPrefix, $VcVars64, $CMakeExe,
        $NinjaExe, $PythonExe,
        $GitExe, $extractor)) {
    Assert-OrdinaryAncestors $path
}
foreach ($file in @($VcVars64, $CMakeExe, $NinjaExe, $PythonExe,
        $GitExe, $extractor)) {
    $item = Get-Item -LiteralPath $file -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo]) { throw "Required tool is not a file: $file" }
}
foreach ($directory in @($companion, $MsysBin, $QtPrefix,
        $ShaderToolsPrefix, $VulkanSdkPrefix)) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        throw "Required input directory is missing: $directory"
    }
}
foreach ($directory in @($companion, $MsysBin, $QtPrefix,
        $ShaderToolsPrefix, $VulkanSdkPrefix)) {
    $inputRoot = [IO.Path]::GetFullPath($directory).TrimEnd('\')
    if ($root.StartsWith($inputRoot + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The fresh output directory must be outside every input tree.'
    }
}
Assert-Hash (Join-Path $companion 'checksums.sha256') $checksumListHash
$checksumEntries = [ordered]@{}
foreach ($line in Get-Content -LiteralPath (Join-Path $companion 'checksums.sha256')) {
    if ($line -cnotmatch '^([A-F0-9]{64})  ([A-Za-z0-9._/-]+)$') {
        throw 'The reviewed companion checksum list has an invalid entry.'
    }
    if ($checksumEntries.Contains($Matches[2])) { throw 'Duplicate companion path.' }
    $checksumEntries[$Matches[2]] = $Matches[1]
}
if ($checksumEntries.Count -ne $sourceFiles.Count -or
        (@($checksumEntries.Keys | Sort-Object) -join '|') -cne
            (@($sourceFiles | Sort-Object) -join '|')) {
    throw 'The reviewed companion file inventory differs.'
}
foreach ($relative in $sourceFiles) {
    Assert-Hash (Join-Path $companion $relative.Replace('/', '\')) `
        $checksumEntries[$relative]
}
$allItems = @(Get-ChildItem -LiteralPath $companion -Recurse -Force)
if (@($allItems | Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        }).Count -ne 0) {
    throw 'Companion contains a reparse-backed item.'
}
$actualFiles = @($allItems | Where-Object { $_ -is [IO.FileInfo] } |
    ForEach-Object {
        [IO.Path]::GetRelativePath($companion, $_.FullName).Replace('\', '/')
    } | Sort-Object)
$wantedFiles = @($sourceFiles + 'checksums.sha256' | Sort-Object)
if (($actualFiles -join '|') -cne ($wantedFiles -join '|')) {
    throw 'Companion contains an extra or missing file.'
}
foreach ($entry in $archiveHashes.GetEnumerator()) {
    if ($checksumEntries[$entry.Key] -cne $entry.Value) {
        throw 'A source archive differs from its pinned identity.'
    }
}
foreach ($entry in $toolHashes.GetEnumerator()) {
    Assert-Hash (Join-Path $MsysBin $entry.Key) $entry.Value
}
foreach ($relative in @('lib/cmake/Qt6/Qt6Config.cmake',
        'bin/Qt6Core.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $QtPrefix $relative) -PathType Leaf)) {
        throw "Required Qt kit input is missing: $relative"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $ShaderToolsPrefix `
        'lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake') -PathType Leaf)) {
    throw 'Matching Qt ShaderTools configuration is missing.'
}
$vulkanHeader = Join-Path $VulkanSdkPrefix 'Include/vulkan/vulkan.h'
$vulkanLibrary = Join-Path $VulkanSdkPrefix 'Lib/vulkan-1.lib'
Assert-Hash $vulkanHeader $VulkanHeaderSha256.ToUpperInvariant()
Assert-Hash $vulkanLibrary $VulkanImportLibrarySha256.ToUpperInvariant()

[IO.Directory]::CreateDirectory($root) | Out-Null
foreach ($relative in @('logs', 'reports', 'sources')) {
    [IO.Directory]::CreateDirectory((Join-Path $root $relative)) | Out-Null
}
$extractCases = @(
    @{ key='zlib'; archive='source/zlib-1.3.1.tar.gz'; sourceRoot='zlib-1.3.1' },
    @{ key='ffmpeg'; archive='source/ffmpeg-n7.1.5.tar.gz'; sourceRoot='FFmpeg-n7.1.5' },
    @{ key='qt'; archive='source/qtmultimedia-everywhere-src-6.11.2.tar.xz'; sourceRoot='qtmultimedia-everywhere-src-6.11.2' }
)
foreach ($case in $extractCases) {
    $archive = Join-Path $companion $case.archive.Replace('/', '\')
    $destination = Join-Path $root "sources/$($case.key)"
    $report = Join-Path $root "reports/$($case.key)-extraction.json"
    Invoke-Bounded "extract-$($case.key)" $PythonExe @(
        $extractor, '--archive', $archive,
        '--sha256', $archiveHashes[$case.archive],
        '--destination', $destination,
        '--source-root', $case.sourceRoot,
        '--report', $report
    ) 600
}

$zlibSource = Join-Path $root 'sources/zlib/zlib-1.3.1'
$zlibCmd = Join-Path $root 'build-zlib.cmd'
@(
    '@echo off',
    "call `"$VcVars64`" -vcvars_ver=$ExpectedVcToolsVersion >nul",
    'if errorlevel 1 exit /b 10',
    "if /I not `"%VCToolsVersion%`"==`"$ExpectedVcToolsVersion`" exit /b 14",
    "cd /d `"$zlibSource`"",
    'if errorlevel 1 exit /b 11',
    'nmake -f win32\Makefile.msc zlib.lib',
    'if errorlevel 1 exit /b 12',
    'nmake -f win32\Makefile.msc test',
    'if errorlevel 1 exit /b 13',
    'exit /b 0'
) | Set-Content -LiteralPath $zlibCmd -Encoding ascii
Invoke-CmdFile 'zlib-build-and-test' $zlibCmd 600
$interop = Join-Path $root 'zlib-interop'
[IO.Directory]::CreateDirectory($interop) | Out-Null
foreach ($name in @('zlib.lib', 'zlib.h', 'zconf.h')) {
    Copy-Item -LiteralPath (Join-Path $zlibSource $name) `
        -Destination (Join-Path $interop $name) -ErrorAction Stop
}
Assert-Hash (Join-Path $interop 'zconf.h') `
    'F5134250A67D57459234B63858F0D9D3EF8DCC48E9E1028D3F4FDCF6EAE677AE'
& $GitExe -C $interop -c core.autocrlf=input -c core.eol=lf apply `
    --unidiff-zero -p1 (Join-Path $companion 'patches/zconf.h.patch')
if ($LASTEXITCODE -ne 0) { throw 'The reviewed zlib interop patch failed.' }
Assert-Hash (Join-Path $interop 'zconf.h') `
    '132778E37C0B74BFE9F0A4B254FD8675E26543B2C1B69DD4A54FA4318745BAC3'

$ffmpegSource = Join-Path $root 'sources/ffmpeg/FFmpeg-n7.1.5'
$ffmpegBuild = Join-Path $root 'ffmpeg-build'
$ffmpegOut = Join-Path $root 'ffmpeg-output'
$ffmpegSh = Join-Path $root 'build-ffmpeg.sh'
$ffmpegCmd = Join-Path $root 'build-ffmpeg.cmd'
$sourcePosix = Convert-ToMsysPath $ffmpegSource
$buildPosix = Convert-ToMsysPath $ffmpegBuild
$outputPosix = Convert-ToMsysPath $ffmpegOut
$interopUnix = $interop.Replace('\', '/')
$options = @('--disable-programs', '--disable-doc', '--disable-debug',
    '--enable-network', '--disable-lzma', '--enable-pic', '--disable-vulkan',
    '--disable-v4l2-m2m', '--disable-decoder=truemotion1', '--disable-avdevice',
    '--disable-avfilter', '--enable-zlib',
    "--extra-cflags=-I$interopUnix", "--extra-ldflags=-LIBPATH:$interopUnix",
    '--toolchain=msvc', '--enable-shared', '--disable-static')
@(
    '#!/usr/bin/env bash',
    'set -euo pipefail',
    "mkdir -p '$buildPosix'",
    "cd '$buildPosix'",
    ((@("'$sourcePosix/configure'", "--prefix='$outputPosix'") +
        $options) -join ' '),
    'make -j2',
    'make install'
) | Set-Content -LiteralPath $ffmpegSh -Encoding ascii
@(
    '@echo off',
    "call `"$VcVars64`" -vcvars_ver=$ExpectedVcToolsVersion >nul",
    'if errorlevel 1 exit /b 10',
    "if /I not `"%VCToolsVersion%`"==`"$ExpectedVcToolsVersion`" exit /b 14",
    'set "MSYS2_PATH_TYPE=inherit"',
    'set "MSYSTEM=MSYS"',
    "set `"PATH=$MsysBin;%PATH%`"",
    "`"$(Join-Path $MsysBin 'bash.exe')`" --noprofile --norc `"$ffmpegSh`"",
    'if errorlevel 1 exit /b 11',
    'exit /b 0'
) | Set-Content -LiteralPath $ffmpegCmd -Encoding ascii
Invoke-CmdFile 'ffmpeg-build' $ffmpegCmd 3600
$actualDlls = @(Get-ChildItem -LiteralPath (Join-Path $ffmpegOut 'bin') `
    -File -Filter '*.dll' | ForEach-Object { $_.Name } | Sort-Object)
if (($actualDlls -join '|') -cne (@($ffmpegNames | Sort-Object) -join '|')) {
    throw 'The FFmpeg build did not produce exactly the five expected DLLs.'
}
foreach ($name in @('avcodec', 'avformat', 'avutil', 'swresample', 'swscale')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ffmpegOut "bin/$name.lib") -PathType Leaf)) {
        throw "The FFmpeg import library is missing: $name"
    }
}

$qtSource = Join-Path $root 'sources/qt/qtmultimedia-everywhere-src-6.11.2'
& $GitExe -C $qtSource -c core.autocrlf=input -c core.eol=lf apply `
    --unidiff-zero -p1 (Join-Path $companion 'patches/qt-ffmpeg-attribution.patch')
if ($LASTEXITCODE -ne 0) { throw 'The reviewed Qt attribution patch failed.' }
Assert-Hash (Join-Path $qtSource 'src/3rdparty/ffmpeg/qt_attribution.json') `
    '24D5FFF2E100FB7C2531771D594A008992F85406387EFC9BA6889D5C51EFC343'
$qtBuild = Join-Path $root 'qt-build'
$qtOut = Join-Path $root 'qt-output'
$qtConfigure = Join-Path $root 'configure-qt.cmd'
$qtCompile = Join-Path $root 'build-qt.cmd'
$env:VULKAN_SDK = $VulkanSdkPrefix
$shaderConfig = Join-Path $ShaderToolsPrefix `
    'lib/cmake/Qt6ShaderTools'
$qtPath = "$($ShaderToolsPrefix)\bin;$QtPrefix\bin"
@(
    '@echo off',
    "call `"$VcVars64`" -vcvars_ver=$ExpectedVcToolsVersion >nul",
    'if errorlevel 1 exit /b 10',
    "if /I not `"%VCToolsVersion%`"==`"$ExpectedVcToolsVersion`" exit /b 14",
    "set `"PATH=$qtPath;%PATH%`"",
    "`"$CMakeExe`" -S `"$qtSource`" -B `"$qtBuild`" -G Ninja `"-DCMAKE_MAKE_PROGRAM=$NinjaExe`" -DCMAKE_BUILD_TYPE=Release `"-DCMAKE_PREFIX_PATH=$ShaderToolsPrefix;$QtPrefix`" `"-DQt6ShaderTools_DIR=$shaderConfig`" `"-DFFMPEG_DIR=$ffmpegOut`" `"-DVulkan_INCLUDE_DIR=$(Join-Path $VulkanSdkPrefix 'Include')`" `"-DVulkan_LIBRARY=$vulkanLibrary`" -DQT_DEPLOY_FFMPEG=OFF -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF `"-DCMAKE_INSTALL_PREFIX=$qtOut`"",
    'exit /b %errorlevel%'
) | Set-Content -LiteralPath $qtConfigure -Encoding ascii
@(
    '@echo off',
    "call `"$VcVars64`" -vcvars_ver=$ExpectedVcToolsVersion >nul",
    'if errorlevel 1 exit /b 10',
    "if /I not `"%VCToolsVersion%`"==`"$ExpectedVcToolsVersion`" exit /b 14",
    "set `"PATH=$qtPath;%PATH%`"",
    "`"$CMakeExe`" --build `"$qtBuild`" --target QFFmpegMediaPlugin --parallel 2",
    'exit /b %errorlevel%'
) | Set-Content -LiteralPath $qtCompile -Encoding ascii
Invoke-CmdFile 'qt-configure' $qtConfigure 600
$qtCache = Join-Path $qtBuild 'CMakeCache.txt'
$cacheLines = @(Get-Content -LiteralPath $qtCache -ErrorAction Stop)
foreach ($entry in @(
        @{ name='Vulkan_INCLUDE_DIR'; expected=(Join-Path $VulkanSdkPrefix 'Include') },
        @{ name='Vulkan_LIBRARY'; expected=$vulkanLibrary }
    )) {
    $cacheEntries = @($cacheLines | Where-Object {
        $_.StartsWith($entry.name + ':', [StringComparison]::Ordinal)
    })
    if ($cacheEntries.Count -ne 1 -or $cacheEntries[0].IndexOf('=') -lt 0) {
        throw 'Qt CMake did not record exactly one pinned Vulkan build input.'
    }
    $actual = $cacheEntries[0].Substring($cacheEntries[0].IndexOf('=') + 1)
    if ([IO.Path]::GetFullPath($actual) -ne
            [IO.Path]::GetFullPath($entry.expected)) {
        throw 'Qt CMake selected a different Vulkan build input.'
    }
}
Invoke-CmdFile 'qt-build' $qtCompile 3600

$plugins = @(Get-ChildItem -LiteralPath $qtBuild -Recurse -File `
    -Filter 'ffmpegmediaplugin.dll')
$modules = @(Get-ChildItem -LiteralPath $qtBuild -Recurse -File `
    -Filter 'Qt6Multimedia.dll')
if ($plugins.Count -ne 1 -or $modules.Count -lt 1) {
    throw 'The Qt build did not produce one plugin and a Multimedia module.'
}
$moduleHashes = @($modules | ForEach-Object {
    (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
} | Select-Object -Unique)
if ($moduleHashes.Count -ne 1) {
    throw 'Qt produced differing Multimedia module bytes in the build tree.'
}
$stage = Join-Path $root 'seven-media-stage'
[IO.Directory]::CreateDirectory((Join-Path $stage 'multimedia')) | Out-Null
$mediaSources = [ordered]@{}
foreach ($name in $ffmpegNames) {
    $mediaSources[$name] = Join-Path $ffmpegOut "bin/$name"
}
$mediaSources['Qt6Multimedia.dll'] = $modules[0].FullName
$mediaSources['multimedia/ffmpegmediaplugin.dll'] = $plugins[0].FullName
$mediaHashes = [ordered]@{}
foreach ($relative in $mediaSources.Keys) {
    $target = Join-Path $stage $relative.Replace('/', '\')
    Copy-Item -LiteralPath $mediaSources[$relative] -Destination $target `
        -ErrorAction Stop
    $sourceHash = (Get-FileHash -LiteralPath $mediaSources[$relative] `
        -Algorithm SHA256).Hash
    Assert-Hash $target $sourceHash
    $mediaHashes[$relative] = $sourceHash
}
$result = [ordered]@{
    schema = 'file-manager-media-source-build-v1'
    status = 'built-unreviewed'
    buildRecipeSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    sourceCompanionSha256 = $checksumListHash
    ffmpegSourceSha256 = $archiveHashes['source/ffmpeg-n7.1.5.tar.gz']
    qtMultimediaSourceSha256 = $archiveHashes['source/qtmultimedia-everywhere-src-6.11.2.tar.xz']
    vulkanSdkVersion = '1.4.357.0'
    vulkanHeaderSha256 = $VulkanHeaderSha256.ToUpperInvariant()
    vulkanImportLibrarySha256 = $VulkanImportLibrarySha256.ToUpperInvariant()
    zlibSourceSha256 = $archiveHashes['source/zlib-1.3.1.tar.gz']
    zlibInteropHeaderSha256 = (Get-FileHash -LiteralPath `
        (Join-Path $interop 'zconf.h') -Algorithm SHA256).Hash
    ffmpegConfigureSha256 = (Get-FileHash -LiteralPath `
        (Join-Path $ffmpegBuild 'ffbuild/config.mak') -Algorithm SHA256).Hash
    qtAttributionSha256 = (Get-FileHash -LiteralPath `
        (Join-Path $qtSource 'src/3rdparty/ffmpeg/qt_attribution.json') `
        -Algorithm SHA256).Hash
    qtCMakeCacheSha256 = (Get-FileHash -LiteralPath `
        (Join-Path $qtBuild 'CMakeCache.txt') -Algorithm SHA256).Hash
    qtNinjaGraphSha256 = (Get-FileHash -LiteralPath `
        (Join-Path $qtBuild 'build.ninja') -Algorithm SHA256).Hash
    qtKitCoreSha256 = (Get-FileHash -LiteralPath `
        (Join-Path $QtPrefix 'bin/Qt6Core.dll') -Algorithm SHA256).Hash
    shaderToolsConfigSha256 = (Get-FileHash -LiteralPath `
        (Join-Path $ShaderToolsPrefix `
            'lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake') `
        -Algorithm SHA256).Hash
    vcToolsVersion = $ExpectedVcToolsVersion
    vcVars64Sha256 = (Get-FileHash -LiteralPath $VcVars64 -Algorithm SHA256).Hash
    msysToolsSha256 = $toolHashes
    cmakeSha256 = (Get-FileHash -LiteralPath $CMakeExe -Algorithm SHA256).Hash
    ninjaSha256 = (Get-FileHash -LiteralPath $NinjaExe -Algorithm SHA256).Hash
    pythonSha256 = (Get-FileHash -LiteralPath $PythonExe -Algorithm SHA256).Hash
    gitSha256 = (Get-FileHash -LiteralPath $GitExe -Algorithm SHA256).Hash
    sourceExtractorSha256 = (Get-FileHash -LiteralPath $extractor -Algorithm SHA256).Hash
    mediaFiles = $mediaHashes
    releaseApproved = $false
    packageBuilt = $false
}
$result | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $root 'reports/media-build-result.json') `
        -Encoding utf8NoBOM
Write-Output 'Built seven media candidates in a fresh isolated tree; release approval remains false.'
