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
    $OutputFile = Join-Path $repositoryRoot 'artifacts\UnrealMCP-0.2.0-UE5.7-Win64.zip'
}
$OutputFile = [System.IO.Path]::GetFullPath($OutputFile)
if (Test-Path -LiteralPath $OutputFile) {
    throw "Output already exists; choose a fresh path: $OutputFile"
}

$stagingRoot = Join-Path $env:TEMP ('UnrealMCP-Fab-' + [guid]::NewGuid().ToString('N'))
$pluginPackage = Join-Path $stagingRoot 'UnrealMCP'
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null

try {
    & (Join-Path $PSScriptRoot 'build-plugin.ps1') -EngineRoot $EngineRoot -OutputDirectory $pluginPackage

    $intermediate = Join-Path $pluginPackage 'Intermediate'
    if ([System.IO.Directory]::Exists($intermediate)) {
        [System.IO.Directory]::Delete($intermediate, $true)
    }
    $pdb = Join-Path $pluginPackage 'Binaries\Win64\UnrealEditor-UnrealMCP.pdb'
    if ([System.IO.File]::Exists($pdb)) {
        [System.IO.File]::Delete($pdb)
    }

    # Fab requires the standard Code Plugin directory shape even when a plugin
    # intentionally has no UObject content.
    New-Item -ItemType Directory -Path (Join-Path $pluginPackage 'Content') -Force | Out-Null

    $required = @(
        (Join-Path $pluginPackage 'UnrealMCP.uplugin'),
        (Join-Path $pluginPackage 'Source'),
        (Join-Path $pluginPackage 'Content'),
        (Join-Path $pluginPackage 'Config'),
        (Join-Path $pluginPackage 'Resources'),
        (Join-Path $pluginPackage 'Binaries\Win64\UnrealMCPGateway.exe'),
        (Join-Path $pluginPackage 'README.md'),
        (Join-Path $pluginPackage 'README.zh-CN.md'),
        (Join-Path $pluginPackage 'docs\architecture.md'),
        (Join-Path $pluginPackage 'docs\capability-coverage.md'),
        (Join-Path $pluginPackage 'docs\tool-minimization.md'),
        (Join-Path $pluginPackage 'LICENSE'),
        (Join-Path $pluginPackage 'THIRD_PARTY_NOTICES.md')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Fab package is missing required path: $path"
        }
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
    $resolvedTemp = [System.IO.Path]::GetFullPath($stagingRoot)
    $tempRoot = [System.IO.Path]::GetFullPath($env:TEMP)
    if ($resolvedTemp.StartsWith($tempRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Directory]::Exists($resolvedTemp)) {
        [System.IO.Directory]::Delete($resolvedTemp, $true)
    }
}
