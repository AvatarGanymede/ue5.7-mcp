param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.7',

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'

Write-Warning 'Repository Binaries are not release inputs. Building a fresh GitHub package with UHT/UBT instead.'
$buildScript = Join-Path $PSScriptRoot 'build-github-package.ps1'
$buildArguments = @{
    EngineRoot = $EngineRoot
}

if (-not [string]::IsNullOrWhiteSpace($OutputFile)) {
    $buildArguments.OutputFile = $OutputFile
}

& $buildScript @buildArguments
if ($LASTEXITCODE -ne 0) {
    throw "Fresh GitHub package build failed with exit code $LASTEXITCODE"
}
