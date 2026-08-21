param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.7',

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDirectory = (Join-Path $env:TEMP ('ue57-mcp-build-' + [guid]::NewGuid().ToString('N')))
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourcePlugin = Join-Path $repositoryRoot 'ModelContextProtocol'
$uat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'

if (-not (Test-Path -LiteralPath (Join-Path $sourcePlugin 'ModelContextProtocol.uplugin') -PathType Leaf)) {
    throw "Plugin descriptor not found: $(Join-Path $sourcePlugin 'ModelContextProtocol.uplugin')"
}
if (-not (Test-Path -LiteralPath $uat -PathType Leaf)) {
    throw "RunUAT.bat not found below EngineRoot: $uat"
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "OutputDirectory already exists; choose a fresh path: $OutputDirectory"
}

$uatLogFolder = Join-Path (Split-Path -Parent $OutputDirectory) ('.unrealmcp-uat-logs-' + (Split-Path -Leaf $OutputDirectory))
$userProfileFolder = Join-Path (Split-Path -Parent $OutputDirectory) ('.unrealmcp-userprofile-' + (Split-Path -Leaf $OutputDirectory))
$localAppDataFolder = Join-Path $userProfileFolder 'AppData\Local'
$roamingAppDataFolder = Join-Path $userProfileFolder 'AppData\Roaming'
$env:uebp_LogFolder = $uatLogFolder
$env:uebp_FinalLogFolder = $uatLogFolder
$env:LOCALAPPDATA = $localAppDataFolder
$env:APPDATA = $roamingAppDataFolder
$env:USERPROFILE = $userProfileFolder
$env:UnrealBuildTool_BuildConfiguration__bAllowUBAExecutor = 'false'
$env:UnrealBuildTool_BuildConfiguration__bAllowXGE = 'false'
$env:UnrealBuildTool_BuildConfiguration__bAllowSNDBS = 'false'
$env:UnrealBuildTool_BuildConfiguration__bAllowFASTBuild = 'false'

$sourceStagingRoot = Join-Path $env:TEMP ('UnrealMCP-BuildSource-' + [guid]::NewGuid().ToString('N'))
$stagedPlugin = Join-Path $sourceStagingRoot 'ModelContextProtocol'
New-Item -ItemType Directory -Path $stagedPlugin -Force | Out-Null

try {
    $excludedTopLevelNames = @('Binaries', 'Intermediate', 'DerivedDataCache', 'Saved')
    foreach ($item in Get-ChildItem -LiteralPath $sourcePlugin -Force) {
        if ($item.Name -notin $excludedTopLevelNames) {
            Copy-Item -LiteralPath $item.FullName -Destination $stagedPlugin -Recurse -Force
        }
    }

    $plugin = Join-Path $stagedPlugin 'ModelContextProtocol.uplugin'
    & $uat BuildPlugin "-Plugin=$plugin" "-Package=$OutputDirectory" '-TargetPlatforms=Win64' '-Rocket'
    if ($LASTEXITCODE -ne 0) {
        throw "UE plugin build failed with exit code $LASTEXITCODE"
    }
}
finally {
    $resolvedStaging = [System.IO.Path]::GetFullPath($sourceStagingRoot)
    $resolvedTemp = [System.IO.Path]::GetFullPath($env:TEMP)
    if ($resolvedStaging.StartsWith($resolvedTemp + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Directory]::Exists($resolvedStaging)) {
        [System.IO.Directory]::Delete($resolvedStaging, $true)
    }
}

# Keep installed/Fab packages self-documenting. These repository-level files are
# outside the .uplugin source directory, so UAT cannot copy them on its own.
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') -Destination (Join-Path $OutputDirectory 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.zh-CN.md') -Destination (Join-Path $OutputDirectory 'README.zh-CN.md') -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'docs') -Destination (Join-Path $OutputDirectory 'docs') -Recurse -Force

Write-Output "Built plugin package: $OutputDirectory"
