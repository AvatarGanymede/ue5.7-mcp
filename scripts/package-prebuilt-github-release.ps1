param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$arguments = @((Join-Path $PSScriptRoot 'package-prebuilt-github-release.sh'))
if (-not [string]::IsNullOrWhiteSpace($OutputFile)) {
    $arguments += @('--output', $OutputFile)
}

& bash @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Prebuilt GitHub package failed with exit code $LASTEXITCODE"
}
