param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.7',

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$pluginRoot = Join-Path $repositoryRoot 'UnrealMCP'
if (-not $OutputPath) {
    $OutputPath = Join-Path $pluginRoot 'Binaries\Win64\UnrealMCPGateway.exe'
}
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
$rapidJson = Join-Path $EngineRoot 'Engine\Source\ThirdParty\RapidJSON\1.1.0'
$sourceRoot = Join-Path $pluginRoot 'Source\Programs\UnrealMCPGateway'

if (-not (Test-Path -LiteralPath (Join-Path $rapidJson 'rapidjson\document.h') -PathType Leaf)) {
    throw "UE RapidJSON headers not found: $rapidJson"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$visualStudio = & $vswhere -latest -products * -property installationPath
if (-not $visualStudio) {
    throw 'A Visual Studio installation with the C++ toolchain was not found.'
}
$devCmd = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) {
    throw "VsDevCmd.bat not found: $devCmd"
}

# Import the compiler environment into this PowerShell process. cmd.exe is used
# only to evaluate Microsoft's environment setup batch file; compilation is
# invoked directly below.
$environmentLines = & $env:ComSpec /d /s /c "`"$devCmd`" -no_logo -arch=amd64 >nul && set"
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [System.Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
    }
}

$outputDirectory = Split-Path -Parent $OutputPath
$intermediate = Join-Path $repositoryRoot 'Intermediate\NativeGateway'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $intermediate -Force | Out-Null

$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$arguments = @(
    '/nologo', '/std:c++20', '/EHsc', '/O2', '/MT', '/W4',
    '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX',
    "/I$rapidJson",
    "/Fo$intermediate\",
    "/Fd$intermediate\UnrealMCPGateway.pdb",
    "/Fe$OutputPath",
    (Join-Path $sourceRoot 'main.cpp'),
    (Join-Path $sourceRoot 'WorkerHttpClient.cpp'),
    '/link', 'winhttp.lib', 'ole32.lib', '/SUBSYSTEM:CONSOLE'
)

& $compiler @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Native gateway compilation failed with exit code $LASTEXITCODE"
}

Get-Item -LiteralPath $OutputPath | Select-Object FullName, Length, LastWriteTime
