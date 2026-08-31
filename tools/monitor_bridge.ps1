[CmdletBinding()]
param(
    [ValidateRange(0, 120)]
    [int]$WaitSeconds = 15
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$pioCommand = Get-Command pio -ErrorAction SilentlyContinue

if ($null -ne $pioCommand) {
    $pioExe = $pioCommand.Source
} else {
    $pioExe = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
}

if (-not (Test-Path -LiteralPath $pioExe)) {
    throw 'PlatformIO Core nao encontrado. Instale o PlatformIO ou adicione pio.exe ao PATH.'
}

$pythonExe = Join-Path (Split-Path -Parent $pioExe) 'python.exe'
$finder = Join-Path $PSScriptRoot 'find_bridge_port.py'
$port = (& $pythonExe $finder --wait $WaitSeconds).Trim()

if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($port)) {
    throw 'A porta COM do Blue Mechanic nao foi encontrada.'
}

Write-Host "Blue Mechanic detectado em $port"
& $pioExe device monitor --project-dir $projectRoot --environment teensy41 --port $port
exit $LASTEXITCODE
