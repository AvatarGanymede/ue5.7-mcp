param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.7',

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputFile) {
    $OutputFile = Join-Path $repositoryRoot 'artifacts\UnrealMCP-0.5.0-UE5.7-Win64-GitHub.zip'
}
$OutputFile = [System.IO.Path]::GetFullPath($OutputFile)
if (Test-Path -LiteralPath $OutputFile) {
    throw "Output already exists; choose a fresh path: $OutputFile"
}

$stagingRoot = Join-Path $env:TEMP ('UnrealMCP-GitHub-' + [guid]::NewGuid().ToString('N'))
$pluginsDirectory = Join-Path $stagingRoot 'Plugins'
$pluginPackage = Join-Path $pluginsDirectory 'ModelContextProtocol'
New-Item -ItemType Directory -Path $pluginsDirectory -Force | Out-Null

try {
    & (Join-Path $PSScriptRoot 'build-plugin.ps1') -EngineRoot $EngineRoot -OutputDirectory $pluginPackage

    $intermediate = Join-Path $pluginPackage 'Intermediate'
    if ([System.IO.Directory]::Exists($intermediate)) {
        [System.IO.Directory]::Delete($intermediate, $true)
    }
    $pdb = Join-Path $pluginPackage 'Binaries\Win64\UnrealEditor-ModelContextProtocol.pdb'
    if ([System.IO.File]::Exists($pdb)) {
        [System.IO.File]::Delete($pdb)
    }

    $required = @(
        (Join-Path $pluginPackage 'ModelContextProtocol.uplugin'),
        (Join-Path $pluginPackage 'Binaries\Win64\UnrealEditor-ModelContextProtocol.dll'),
        (Join-Path $pluginPackage 'Source'),
        (Join-Path $pluginPackage 'Config'),
        (Join-Path $pluginPackage 'Resources'),
        (Join-Path $pluginPackage 'README.md'),
        (Join-Path $pluginPackage 'README.zh-CN.md'),
        (Join-Path $pluginPackage 'LICENSE')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "GitHub package is missing required path: $path"
        }
    }

    $unexpectedExecutables = @(Get-ChildItem -LiteralPath $pluginPackage -Recurse -File -Filter '*.exe')
    if ($unexpectedExecutables.Count -gt 0) {
        throw "GitHub package unexpectedly contains an EXE: $($unexpectedExecutables[0].FullName)"
    }

    $outputDirectory = Split-Path -Parent $OutputFile
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $stagingRoot,
        $OutputFile,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)

    Get-Item -LiteralPath $OutputFile | Select-Object FullName, Length, LastWriteTime
}
finally {
    $resolvedStaging = [System.IO.Path]::GetFullPath($stagingRoot)
    $resolvedTemp = [System.IO.Path]::GetFullPath($env:TEMP)
    if ($resolvedStaging.StartsWith($resolvedTemp + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Directory]::Exists($resolvedStaging)) {
        [System.IO.Directory]::Delete($resolvedStaging, $true)
    }
}
