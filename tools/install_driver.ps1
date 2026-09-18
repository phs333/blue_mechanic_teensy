[CmdletBinding()]
param()

# Auto-elevação para Administrador
$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$currentIdentity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Solicitando privilegios de Administrador..."
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

Write-Host "========================================================"
Write-Host "  Configurando Nome: Azul Mecanico USB"
Write-Host "========================================================"
Write-Host ""

$targetName = "Azul Mecanico USB"
$found = $false

# 1. Configura a porta COM virtual (interface CDC ACM MI_00)
$cdcPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_1209&PID_0001&MI_00"
if (Test-Path $cdcPath) {
    Get-ChildItem -Path $cdcPath | ForEach-Object {
        $instancePath = $_.PSPath
        $paramsPath = Join-Path $instancePath "Device Parameters"
        $port = ""
        if (Test-Path $paramsPath) {
            $port = (Get-ItemProperty -Path $paramsPath -Name "PortName" -ErrorAction SilentlyContinue).PortName
        }
        if (-not $port) {
            $currentFriendly = (Get-ItemProperty -Path $instancePath -Name "FriendlyName" -ErrorAction SilentlyContinue).FriendlyName
            if ($currentFriendly -match '\((COM\d+)\)') {
                $port = $matches[1]
            }
        }

        $friendlyName = if ($port) { "$targetName ($port)" } else { $targetName }

        Set-ItemProperty -Path $instancePath -Name "FriendlyName" -Value $friendlyName
        Set-ItemProperty -Path $instancePath -Name "DeviceDesc" -Value $targetName
        Write-Host "[OK] Porta COM configurada: $friendlyName" -ForegroundColor Green
        $found = $true
    }
}

# 2. Configura o dispositivo pai USB Composite Device
$parentPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_1209&PID_0001"
if (Test-Path $parentPath) {
    Get-ChildItem -Path $parentPath | ForEach-Object {
        $parentInstancePath = $_.PSPath
        if ($_.PSChildName -ne "Device Parameters") {
            Set-ItemProperty -Path $parentInstancePath -Name "FriendlyName" -Value $targetName
            Set-ItemProperty -Path $parentInstancePath -Name "DeviceDesc" -Value $targetName
            Write-Host "[OK] Dispositivo USB configurado: $targetName" -ForegroundColor Green
            $found = $true
        }
    }
}

# 3. Reinicia os dispositivos PnP do Teensy para atualizar o Gerenciador de Dispositivos na hora
$pnpDevices = Get-PnpDevice | Where-Object { $_.InstanceId -like "*VID_1209&PID_0001*" }
if ($pnpDevices) {
    Write-Host "`nAtualizando dispositivo no Gerenciador de Dispositivos..."
    foreach ($dev in $pnpDevices) {
        try {
            Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 300
            Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        } catch {
            # Se não conseguir desativar/reativar dinamicamente, basta reconectar o cabo
        }
    }
}

Write-Host ""
if ($found) {
    Write-Host "========================================================" -ForegroundColor Green
    Write-Host " SUCESSO: Renomeado para 'Azul Mecanico USB'!" -ForegroundColor Green
    Write-Host " O Gerenciador de Dispositivos ja foi atualizado." -ForegroundColor Green
    Write-Host "========================================================" -ForegroundColor Green
} else {
    Write-Warning "Nenhum Teensy com VID:PID 1209:0001 foi encontrado no registro."
    Write-Warning "Certifique-se de que o Teensy 4.1 esteja plugado com o firmware gravado."
}

Write-Host ""
Read-Host -Prompt "Pressione Enter para sair..."
