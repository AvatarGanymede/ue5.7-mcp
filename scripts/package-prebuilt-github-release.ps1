param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourcePlugin = Join-Path $repositoryRoot 'ModelContextProtocol'
$descriptorPath = Join-Path $sourcePlugin 'ModelContextProtocol.uplugin'

if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) {
    throw "Plugin descriptor not found: $descriptorPath"
}

$descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
$version = [string]$descriptor.VersionName
if ([string]::IsNullOrWhiteSpace($version)) {
    throw "VersionName is missing from: $descriptorPath"
}

if (-not $OutputFile) {
    $OutputFile = Join-Path $repositoryRoot "artifacts\UnrealMCP-$version-UE5.7-Win64-GitHub.zip"
}
$OutputFile = [System.IO.Path]::GetFullPath($OutputFile)
if (Test-Path -LiteralPath $OutputFile) {
    throw "Output already exists; choose a fresh path: $OutputFile"
}

$stagingRoot = Join-Path $env:TEMP ('UnrealMCP-Prebuilt-GitHub-' + [guid]::NewGuid().ToString('N'))
$pluginsDirectory = Join-Path $stagingRoot 'Plugins'
$pluginPackage = Join-Path $pluginsDirectory 'ModelContextProtocol'
New-Item -ItemType Directory -Path $pluginsDirectory -Force | Out-Null

try {
    Copy-Item -LiteralPath $sourcePlugin -Destination $pluginPackage -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') -Destination (Join-Path $pluginPackage 'README.md') -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.zh-CN.md') -Destination (Join-Path $pluginPackage 'README.zh-CN.md') -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'docs') -Destination (Join-Path $pluginPackage 'docs') -Recurse -Force

    $excludedDirectoryNames = @('Intermediate', 'DerivedDataCache', 'Saved', 'node_modules')
    $excludedDirectories = @(
        Get-ChildItem -LiteralPath $pluginPackage -Directory -Recurse -Force |
            Where-Object { $_.Name -in $excludedDirectoryNames } |
            Sort-Object { $_.FullName.Length } -Descending
    )
    foreach ($directory in $excludedDirectories) {
        if ([System.IO.Directory]::Exists($directory.FullName)) {
            [System.IO.Directory]::Delete($directory.FullName, $true)
        }
    }

    $excludedExtensions = @('.pdb', '.exe', '.obj', '.lib', '.exp', '.ilk')
    $excludedFiles = @(
        Get-ChildItem -LiteralPath $pluginPackage -File -Recurse -Force |
            Where-Object { $_.Extension.ToLowerInvariant() -in $excludedExtensions }
    )
    foreach ($file in $excludedFiles) {
        [System.IO.File]::Delete($file.FullName)
    }

    $required = @(
        (Join-Path $pluginPackage 'ModelContextProtocol.uplugin'),
        (Join-Path $pluginPackage 'Binaries\Win64\UnrealEditor-ModelContextProtocol.dll'),
        (Join-Path $pluginPackage 'Binaries\Win64\UnrealEditor.modules'),
        (Join-Path $pluginPackage 'Source'),
        (Join-Path $pluginPackage 'Config'),
        (Join-Path $pluginPackage 'Resources'),
        (Join-Path $pluginPackage 'README.md'),
        (Join-Path $pluginPackage 'README.zh-CN.md'),
        (Join-Path $pluginPackage 'LICENSE'),
        (Join-Path $pluginPackage 'THIRD_PARTY_NOTICES.md')
    )
    foreach ($path in $required) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Prebuilt GitHub package is missing required path: $path"
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
    $resolvedStaging = [System.IO.Path]::GetFullPath($stagingRoot)
    $resolvedTemp = [System.IO.Path]::GetFullPath($env:TEMP)
    if ($resolvedStaging.StartsWith($resolvedTemp + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Directory]::Exists($resolvedStaging)) {
        [System.IO.Directory]::Delete($resolvedStaging, $true)
    }
}
