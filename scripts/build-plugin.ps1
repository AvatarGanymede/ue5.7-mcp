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
$plugin = Join-Path $repositoryRoot 'UnrealMCP\UnrealMCP.uplugin'
$uat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$nativeBuild = Join-Path $PSScriptRoot 'build-native-gateway.ps1'

if (-not (Test-Path -LiteralPath $plugin -PathType Leaf)) {
    throw "Plugin descriptor not found: $plugin"
}
if (-not (Test-Path -LiteralPath $uat -PathType Leaf)) {
    throw "RunUAT.bat not found below EngineRoot: $uat"
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "OutputDirectory already exists; choose a fresh path: $OutputDirectory"
}

$nativeOutput = Join-Path $env:TEMP ('UnrealMCPGateway-' + [guid]::NewGuid().ToString('N') + '.exe')
& $nativeBuild -EngineRoot $EngineRoot -OutputPath $nativeOutput
if (-not (Test-Path -LiteralPath $nativeOutput -PathType Leaf)) {
    throw 'Native gateway build did not produce an executable.'
}

& $uat BuildPlugin "-Plugin=$plugin" "-Package=$OutputDirectory" '-TargetPlatforms=Win64' '-Rocket'
if ($LASTEXITCODE -ne 0) {
    throw "UE plugin build failed with exit code $LASTEXITCODE"
}

$packageBinaries = Join-Path $OutputDirectory 'Binaries\Win64'
New-Item -ItemType Directory -Path $packageBinaries -Force | Out-Null
Copy-Item -LiteralPath $nativeOutput -Destination (Join-Path $packageBinaries 'UnrealMCPGateway.exe') -Force
[System.IO.File]::Delete($nativeOutput)

# Keep installed/Fab packages self-documenting. These repository-level files are
# outside the .uplugin source directory, so UAT cannot copy them on its own.
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') -Destination (Join-Path $OutputDirectory 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.zh-CN.md') -Destination (Join-Path $OutputDirectory 'README.zh-CN.md') -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'docs') -Destination (Join-Path $OutputDirectory 'docs') -Recurse -Force

Write-Output "Built plugin package: $OutputDirectory"
