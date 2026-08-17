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
$project = Join-Path $repositoryRoot 'UE57MCPTest.uproject'
$gateway = Join-Path $repositoryRoot 'UnrealMCP\Binaries\Win64\UnrealMCPGateway.exe'

if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
    throw "UnrealEditor-Cmd.exe not found: $editor"
}
if (-not (Test-Path -LiteralPath $project -PathType Leaf)) {
    throw "Test host project not found: $project"
}
if (-not (Test-Path -LiteralPath $gateway -PathType Leaf)) {
    throw "Native gateway not found; run scripts\build-native-gateway.ps1 first: $gateway"
}

$testToken = 'ue57mcp-e2e-' + [guid]::NewGuid().ToString('N')
$env:UE_MCP_WORKER_PORT = [string]$Port
$env:UE_MCP_WORKER_TOKEN = $testToken
$process = Start-Process `
    -FilePath $editor `
    -ArgumentList @($project, '-unattended', '-nullrhi', '-nosplash', '-nosound', '-NoAssetRegistryCache', '-log') `
    -PassThru `
    -WindowStyle Hidden
$gatewayProcess = $null

try {
    $headers = @{ Authorization = 'Bearer ' + $testToken }
    $health = $null
    for ($attempt = 0; $attempt -lt 90; $attempt++) {
        if ($process.HasExited) {
            throw "Editor exited early with code $($process.ExitCode)"
        }
        try {
            $health = Invoke-RestMethod `
                -Uri "http://127.0.0.1:$Port/ue-mcp/v1/health" `
                -Headers $headers `
                -TimeoutSec 2
            break
        }
        catch {
            Start-Sleep -Seconds 1
        }
    }
    if ($null -eq $health) {
        throw 'Worker did not become healthy within 90 seconds'
    }

    $gatewayStart = [System.Diagnostics.ProcessStartInfo]::new()
    $gatewayStart.FileName = $gateway
    $gatewayStart.UseShellExecute = $false
    $gatewayStart.CreateNoWindow = $true
    $gatewayStart.RedirectStandardInput = $true
    $gatewayStart.RedirectStandardOutput = $true
    $gatewayStart.RedirectStandardError = $true
    $gatewayProcess = [System.Diagnostics.Process]::new()
    $gatewayProcess.StartInfo = $gatewayStart
    if (-not $gatewayProcess.Start()) {
        throw 'Could not start the native MCP gateway.'
    }

    function Invoke-NativeMcpRequest([hashtable]$Request) {
        $gatewayProcess.StandardInput.WriteLine(($Request | ConvertTo-Json -Depth 16 -Compress))
        $line = $gatewayProcess.StandardOutput.ReadLine()
        if (-not $line) {
            throw 'Native gateway closed stdout before returning an MCP response.'
        }
        return $line | ConvertFrom-Json -Depth 32
    }

    $initialize = Invoke-NativeMcpRequest @{
        jsonrpc = '2.0'
        id = 1
        method = 'initialize'
        params = @{
            protocolVersion = '2025-11-25'
            capabilities = @{}
            clientInfo = @{ name = 'ue57-native-e2e'; version = '1.0.0' }
        }
    }
    $gatewayProcess.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    $tools = Invoke-NativeMcpRequest @{ jsonrpc = '2.0'; id = 2; method = 'tools/list'; params = @{} }
    if ($tools.result.tools.Count -ne 1 -or $tools.result.tools[0].name -ne 'unreal') {
        throw 'Native gateway did not expose exactly one unreal tool.'
    }

    $execute = Invoke-NativeMcpRequest @{
        jsonrpc = '2.0'
        id = 3
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
    if ($execute.result.isError -or -not $execute.result.structuredContent.ok) {
        throw 'Native MCP execute returned an error.'
    }

    [pscustomobject]@{
        health = $health
        protocol = $initialize.result.protocolVersion
        tools = @($tools.result.tools.name)
        execute = $execute.result.structuredContent.data
    } | ConvertTo-Json -Depth 16
}
finally {
    if ($gatewayProcess -and -not $gatewayProcess.HasExited) {
        $gatewayProcess.StandardInput.Close()
        if (-not $gatewayProcess.WaitForExit(3000)) {
            $gatewayProcess.Kill($true)
        }
    }
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id
        Wait-Process -Id $process.Id -Timeout 20 -ErrorAction SilentlyContinue
    }
}
