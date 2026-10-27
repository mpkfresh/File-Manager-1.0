# Shared read-only contract for reviewed Qt Multimedia / FFmpeg bytes.
# Development fixtures may provide six media files; a release-approved
# source-matched Qt plugin also requires its matching Qt6Multimedia.dll.
function Get-ReviewedFfmpegInputs {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Workspace,
        [Parameter(Mandatory)][string]$ManifestPath,
        [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')]
        [string]$ManifestSha256,
        [Parameter(Mandatory)][string]$RuntimeDirectory,
        [switch]$RequireReleaseApproval
    )

    $workspaceRoot = [IO.Path]::GetFullPath($Workspace).TrimEnd('\')
    $manifest = [IO.Path]::GetFullPath((Join-Path $workspaceRoot $ManifestPath))
    if (-not $manifest.StartsWith($workspaceRoot + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Reviewed FFmpeg manifest must be inside the workspace.'
    }
    function Assert-ManifestDirectory([string]$Directory) {
        $item = Get-Item -LiteralPath $Directory -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
            return
        }
        # Source review has already encountered this one Cloud Files directory
        # tag. Require the exact tag; other links and junctions are refused.
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = Join-Path $env:SystemRoot 'System32\fsutil.exe'
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        foreach ($argument in @('reparsepoint', 'query', $Directory)) {
            [void]$info.ArgumentList.Add($argument)
        }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $info
        try {
            if (-not $process.Start()) {
                throw 'Could not inspect reviewed FFmpeg manifest directory tag.'
            }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            $done = $process.WaitForExit(5000)
            if (-not $done) {
                $process.Kill($true)
                $process.WaitForExit()
                throw 'Reviewed FFmpeg manifest directory tag query timed out.'
            }
            $message = $stdout.GetAwaiter().GetResult() +
                $stderr.GetAwaiter().GetResult()
            if ($process.ExitCode -ne 0 -or
                    $message -notmatch 'Reparse Tag Value\s*:\s*(0x[0-9A-Fa-f]+)' -or
                    $Matches[1].ToLowerInvariant() -cne '0x9000601a') {
                throw 'Reviewed FFmpeg manifest crosses an unknown reparse point.'
            }
        } finally {
            $process.Dispose()
        }
    }
    $manifestParent = [IO.Path]::GetDirectoryName($manifest)
    while ($manifestParent.StartsWith($workspaceRoot,
            [StringComparison]::OrdinalIgnoreCase)) {
        Assert-ManifestDirectory $manifestParent
        if ($manifestParent.Equals($workspaceRoot,
                [StringComparison]::OrdinalIgnoreCase)) { break }
        $manifestParent = [IO.Path]::GetDirectoryName($manifestParent)
    }
    $manifestItem = Get-Item -LiteralPath $manifest -Force -ErrorAction Stop
    if ($manifestItem -isnot [IO.FileInfo] -or
            ($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -ine
                $ManifestSha256) {
        throw 'Reviewed FFmpeg manifest is missing, reparse-backed or changed.'
    }
    $record = Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json
    if ($null -eq $record.dlls -or
            @($record.dlls.PSObject.Properties).Count -ne 5 -or
            $record.qtMediaPluginSha256 -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $record.sourceArchiveSha256 -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $record.buildRecordSha256 -cnotmatch '^[A-Fa-f0-9]{64}$') {
        throw 'Reviewed FFmpeg manifest has an incomplete source or media contract.'
    }
    $moduleProperty = $record.PSObject.Properties['qtMultimediaModuleSha256']
    $moduleHash = if ($null -ne $moduleProperty) {
        [string]$moduleProperty.Value
    } else { '' }
    if ($moduleHash -and $moduleHash -cnotmatch '^[A-Fa-f0-9]{64}$') {
        throw 'Reviewed FFmpeg manifest has an invalid Qt Multimedia module hash.'
    }
    $moduleProvenanceProperty = $record.PSObject.Properties['moduleProvenanceCleared']
    $qtSourceProperty = $record.PSObject.Properties['qtMultimediaSourceArchiveSha256']
    $qtBuildProperty = $record.PSObject.Properties['qtMultimediaBuildRecordSha256']
    $qtAttributionProperty = $record.PSObject.Properties['qtMultimediaAttributionSha256']
    $qtCacheProperty = $record.PSObject.Properties['qtMultimediaCMakeCacheSha256']
    $qtGraphProperty = $record.PSObject.Properties['qtMultimediaNinjaGraphSha256']
    $qtRecipeProperty = $record.PSObject.Properties['qtMultimediaBuildRecipeSha256']
    $provenanceProperty = $record.PSObject.Properties['mediaProvenanceSha256']
    $provenanceHash = if ($null -ne $provenanceProperty) {
        [string]$provenanceProperty.Value
    } else { '' }
    if ($provenanceHash -and $provenanceHash -cnotmatch '^[A-Fa-f0-9]{64}$') {
        throw 'Reviewed FFmpeg manifest has an invalid media provenance hash.'
    }
    if ($RequireReleaseApproval -and
            ($record.version -cne '7.1.5' -or
            $record.sourceArchiveSha256 -ine
                'E3963A50831C985933E1A625ED566EC4C7ADB5C012C34FA9F84438E1D61BDACC' -or
            $record.status -cne 'release-approved' -or
            $record.releaseApproved -isnot [bool] -or
            $record.releaseApproved -ne $true -or
            $record.pluginProvenanceCleared -isnot [bool] -or
            $record.pluginProvenanceCleared -ne $true -or
            $moduleHash -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $null -eq $qtSourceProperty -or
            [string]$qtSourceProperty.Value -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $null -eq $qtBuildProperty -or
            [string]$qtBuildProperty.Value -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $null -eq $qtAttributionProperty -or
            [string]$qtAttributionProperty.Value -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $null -eq $qtCacheProperty -or
            [string]$qtCacheProperty.Value -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $null -eq $qtGraphProperty -or
            [string]$qtGraphProperty.Value -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $null -eq $qtRecipeProperty -or
            [string]$qtRecipeProperty.Value -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $provenanceHash -cnotmatch '^[A-Fa-f0-9]{64}$' -or
            $null -eq $moduleProvenanceProperty -or
            $moduleProvenanceProperty.Value -isnot [bool] -or
            $moduleProvenanceProperty.Value -ne $true)) {
        throw 'Version 1.0 or signed packaging requires approved FFmpeg, Qt plugin and matching Qt Multimedia module provenance.'
    }
    if (-not [IO.Path]::IsPathFullyQualified($RuntimeDirectory)) {
        throw 'Reviewed FFmpeg runtime directory must be absolute.'
    }
    $runtimeRoot = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\')
    $ancestor = $runtimeRoot
    while ($ancestor) {
        $item = Get-Item -LiteralPath $ancestor -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reviewed FFmpeg runtime crosses a reparse point: $ancestor"
        }
        $parent = [IO.Path]::GetDirectoryName($ancestor)
        if (-not $parent -or $parent -eq $ancestor) { break }
        $ancestor = $parent
    }
    if (-not (Test-Path -LiteralPath $runtimeRoot -PathType Container)) {
        throw 'Reviewed FFmpeg runtime directory is missing.'
    }

    $hashes = [ordered]@{}
    foreach ($name in @('avcodec-61.dll', 'avformat-61.dll',
            'avutil-59.dll', 'swresample-5.dll', 'swscale-8.dll')) {
        $property = $record.dlls.PSObject.Properties[$name]
        if ($null -eq $property -or
                [string]$property.Value -cnotmatch '^[A-Fa-f0-9]{64}$') {
            throw "Reviewed FFmpeg manifest lacks an exact hash for $name."
        }
        $hashes[$name] = [string]$property.Value
    }
    $hashes['multimedia/ffmpegmediaplugin.dll'] =
        [string]$record.qtMediaPluginSha256
    if ($moduleHash) { $hashes['Qt6Multimedia.dll'] = $moduleHash }
    if ($provenanceHash) {
        $hashes['manifest/media-provenance.json'] = $provenanceHash
    }

    $sources = [ordered]@{}
    foreach ($relative in $hashes.Keys) {
        $source = Join-Path $runtimeRoot $relative.Replace('/', '\')
        $item = Get-Item -LiteralPath $source -Force -ErrorAction Stop
        if ($item -isnot [IO.FileInfo] -or
                ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine
                    $hashes[$relative]) {
            throw "Reviewed FFmpeg runtime differs: $relative"
        }
        $sources[$relative] = $source
    }
    if ($RequireReleaseApproval) {
        if ($record.PSObject.Properties['qtMultimediaSbomSha256']) {
            throw 'The vendor Qt Multimedia SBOM cannot describe the reviewed media.'
        }
    }
    # A review-only candidate with a provenance record must still bind its
    # contents to the seven actual bytes. Release approval is a separate gate.
    if ($provenanceHash) {
        $provenance = Get-Content -Raw -LiteralPath $sources['manifest/media-provenance.json'] |
            ConvertFrom-Json
        $expectedProperties = @('schema', 'ffmpegVersion',
            'qtMultimediaVersion', 'ffmpegSourceArchiveSha256',
            'ffmpegBuildRecordSha256', 'qtMultimediaSourceArchiveSha256',
            'qtMultimediaBuildRecordSha256', 'qtMultimediaAttributionSha256',
            'qtMultimediaCMakeCacheSha256', 'qtMultimediaNinjaGraphSha256',
            'qtMultimediaBuildRecipeSha256',
            'zlibSourceArchiveSha256', 'qtShaderToolsArchiveSha256', 'mediaFiles')
        $actualProperties = @($provenance.PSObject.Properties.Name)
        if ((@($expectedProperties | Sort-Object) -join '|') -cne
                (@($actualProperties | Sort-Object) -join '|') -or
                $provenance.schema -cne 'file-manager-media-provenance-v2' -or
                $provenance.ffmpegVersion -cne '7.1.5' -or
                $provenance.qtMultimediaVersion -cne '6.11.2' -or
                $provenance.ffmpegSourceArchiveSha256 -ine $record.sourceArchiveSha256 -or
                $provenance.ffmpegBuildRecordSha256 -ine $record.buildRecordSha256 -or
                $provenance.qtMultimediaSourceArchiveSha256 -ine $qtSourceProperty.Value -or
                $provenance.qtMultimediaBuildRecordSha256 -ine $qtBuildProperty.Value -or
                $provenance.qtMultimediaAttributionSha256 -ine
                    $record.qtMultimediaAttributionSha256 -or
                $provenance.qtMultimediaCMakeCacheSha256 -ine $qtCacheProperty.Value -or
                $provenance.qtMultimediaNinjaGraphSha256 -ine $qtGraphProperty.Value -or
                $provenance.qtMultimediaBuildRecipeSha256 -ine $qtRecipeProperty.Value -or
                $provenance.zlibSourceArchiveSha256 -ine
                    '9A93B2B7DFDAC77CEBA5A558A580E74667DD6FEDE4585B91EEFB60F03B72DF23' -or
                $provenance.qtShaderToolsArchiveSha256 -ine
                    'D434C320F5788CF54C71E68AB28488CEB2F1BA725E63241530155347A4C5081E') {
            throw 'Reviewed media provenance source/build contract differs.'
        }
        $expectedMedia = @($hashes.Keys | Where-Object {
            $_ -cne 'manifest/media-provenance.json'
        })
        $actualMedia = @($provenance.mediaFiles.PSObject.Properties.Name)
        if ($expectedMedia.Count -ne 7 -or $actualMedia.Count -ne 7 -or
                (@($expectedMedia | Sort-Object) -join '|') -cne
                    (@($actualMedia | Sort-Object) -join '|')) {
            throw 'Reviewed media provenance must list exactly seven media files.'
        }
        foreach ($relative in $expectedMedia) {
            if ($provenance.mediaFiles.PSObject.Properties[$relative].Value -ine
                    $hashes[$relative]) {
                throw "Reviewed media provenance hash differs: $relative"
            }
        }
    }
    return [pscustomobject]@{
        Manifest = $record
        ManifestPath = $manifest
        RuntimeDirectory = $runtimeRoot
        Sources = $sources
        Sha256 = $hashes
    }
}
