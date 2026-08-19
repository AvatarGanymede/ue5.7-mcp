param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.7',

    [Parameter()]
    [ValidateRange(1024, 65535)]
    [int]$Port = 18779
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$stagingRoot = Join-Path $env:TEMP ('UnrealMCP-E2E-' + [guid]::NewGuid().ToString('N'))
$pluginPackage = Join-Path $stagingRoot 'ModelContextProtocol'
$project = Join-Path $stagingRoot 'UE57MCPTest.uproject'

if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
    throw "UnrealEditor-Cmd.exe not found: $editor"
}

New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
$process = $null

try {
    & (Join-Path $PSScriptRoot 'build-plugin.ps1') -EngineRoot $EngineRoot -OutputDirectory $pluginPackage

    $projectDescriptor = [ordered]@{
        FileVersion = 3
        EngineAssociation = '5.7'
        Category = 'Tests'
        Description = 'Generated host for the UnrealMCP Streamable HTTP end-to-end test.'
        AdditionalPluginDirectories = @($pluginPackage)
        Plugins = @(
            [ordered]@{ Name = 'ModelContextProtocol'; Enabled = $true },
            [ordered]@{ Name = 'PythonScriptPlugin'; Enabled = $true }
        )
    }
    [System.IO.File]::WriteAllText(
        $project,
        ($projectDescriptor | ConvertTo-Json -Depth 8),
        [System.Text.UTF8Encoding]::new($false))

    $testToken = 'ue57mcp-e2e-' + [guid]::NewGuid().ToString('N')
    $env:UE_MCP_PORT = [string]$Port
    $env:UE_MCP_TOKEN = $testToken
    $process = Start-Process `
        -FilePath $editor `
        -ArgumentList @($project, '-unattended', '-nullrhi', '-nosplash', '-nosound', '-NoAssetRegistryCache', '-log') `
        -PassThru `
        -WindowStyle Hidden

    $endpoint = "http://127.0.0.1:$Port/mcp"
    $headers = @{
        Authorization = 'Bearer ' + $testToken
        Accept = 'application/json, text/event-stream'
    }

    function Invoke-McpRequest([hashtable]$Request) {
        $json = $Request | ConvertTo-Json -Depth 32 -Compress
        $response = Invoke-WebRequest `
            -Uri $endpoint `
            -Method Post `
            -Headers $headers `
            -ContentType 'application/json' `
            -Body ([System.Text.Encoding]::UTF8.GetBytes($json)) `
            -TimeoutSec 30
        $parsed = if ($response.Content) { $response.Content | ConvertFrom-Json -Depth 64 } else { $null }
        return [pscustomobject]@{ StatusCode = [int]$response.StatusCode; Json = $parsed }
    }

    $initialize = $null
    for ($attempt = 0; $attempt -lt 90; $attempt++) {
        if ($process.HasExited) {
            throw "Editor exited early with code $($process.ExitCode)"
        }
        try {
            $initialize = Invoke-McpRequest @{
                jsonrpc = '2.0'
                id = 1
                method = 'initialize'
                params = @{
                    protocolVersion = '2025-11-25'
                    capabilities = @{}
                    clientInfo = @{ name = 'ue57-http-e2e'; version = '1.0.0' }
                }
            }
            if ($initialize.Json.result) { break }
        }
        catch {
            Start-Sleep -Seconds 1
        }
    }
    if (-not $initialize -or -not $initialize.Json.result) {
        throw 'In-editor MCP server did not become ready within 90 seconds.'
    }

    $initialized = Invoke-McpRequest @{
        jsonrpc = '2.0'
        method = 'notifications/initialized'
        params = @{}
    }
    if ($initialized.StatusCode -ne 202) {
        throw "Initialized notification returned HTTP $($initialized.StatusCode), expected 202."
    }

    $tools = Invoke-McpRequest @{ jsonrpc = '2.0'; id = 2; method = 'tools/list'; params = @{} }
    if ($tools.Json.result.tools.Count -ne 1 -or $tools.Json.result.tools[0].name -ne 'unreal') {
        throw 'In-editor MCP server did not expose exactly one unreal tool.'
    }

    $health = Invoke-McpRequest @{
        jsonrpc = '2.0'
        id = 3
        method = 'tools/call'
        params = @{ name = 'unreal'; arguments = @{ action = 'health' } }
    }
    if ($health.Json.result.isError -or -not $health.Json.result.structuredContent.ok) {
        throw 'MCP health action returned an error.'
    }

    $execute = Invoke-McpRequest @{
        jsonrpc = '2.0'
        id = 4
        method = 'tools/call'
        params = @{
            name = 'unreal'
            arguments = @{
                action = 'execute'
                transaction = $false
                commands = @(
                    @{
                        kind = 'python'
                        mode = 'eval'
                        label = 'engine-version'
                        code = 'unreal.SystemLibrary.get_engine_version()'
                    }
                )
            }
        }
    }
    if ($execute.Json.result.isError -or -not $execute.Json.result.structuredContent.ok) {
        throw 'MCP execute action returned an error.'
    }

    $env:UE_MCP_TEST_URL = $endpoint
    $env:UE_MCP_TEST_TOKEN = $testToken
    & npm test
    if ($LASTEXITCODE -ne 0) {
        throw "Streamable HTTP SDK integration tests failed with exit code $LASTEXITCODE"
    }

    [pscustomobject]@{
        endpoint = $endpoint
        protocol = $initialize.Json.result.protocolVersion
        tools = @($tools.Json.result.tools.name)
        health = $health.Json.result.structuredContent.data
        execute = $execute.Json.result.structuredContent.data
    } | ConvertTo-Json -Depth 16
}
finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id
        Wait-Process -Id $process.Id -Timeout 20 -ErrorAction SilentlyContinue
    }
    Remove-Item Env:UE_MCP_PORT -ErrorAction SilentlyContinue
    Remove-Item Env:UE_MCP_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:UE_MCP_TEST_URL -ErrorAction SilentlyContinue
    Remove-Item Env:UE_MCP_TEST_TOKEN -ErrorAction SilentlyContinue

    $resolvedStaging = [System.IO.Path]::GetFullPath($stagingRoot)
    $resolvedTemp = [System.IO.Path]::GetFullPath($env:TEMP)
    if ($resolvedStaging.StartsWith($resolvedTemp + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Directory]::Exists($resolvedStaging)) {
        $removed = $false
        for ($attempt = 0; $attempt -lt 10 -and -not $removed; $attempt++) {
            try {
                [System.IO.Directory]::Delete($resolvedStaging, $true)
                $removed = $true
            }
            catch [System.IO.IOException] {
                Start-Sleep -Milliseconds 500
            }
            catch [System.UnauthorizedAccessException] {
                Start-Sleep -Milliseconds 500
            }
        }
        if (-not $removed) {
            Write-Warning "Could not remove temporary E2E directory after retries: $resolvedStaging"
        }
    }
}
