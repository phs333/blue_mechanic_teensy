[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$HexPath,

    [Parameter(Mandatory = $true)]
    [string]$LoaderPath
)

$resolvedHex = (Resolve-Path -LiteralPath $HexPath -ErrorAction Stop).Path
$resolvedLoader = (Resolve-Path -LiteralPath $LoaderPath -ErrorAction Stop).Path

Write-Host 'Aguardando o Teensy 4.1. Pressione Program uma vez se solicitado.'
& $resolvedLoader -mmcu=TEENSY41 -w -v $resolvedHex
$uploadExitCode = $LASTEXITCODE

# Algumas combinações Windows/HalfKay recusam a primeira transferência logo
# depois da entrada no bootloader, mas mantêm o dispositivo pronto. Uma única
# repetição imediata conclui a gravação sem novo toque no botão.
if ($uploadExitCode -ne 0) {
    Write-Warning 'Primeira transferência falhou; repetindo com o HalfKay ativo.'
    Start-Sleep -Milliseconds 200
    & $resolvedLoader -mmcu=TEENSY41 -v $resolvedHex
    $uploadExitCode = $LASTEXITCODE
}

exit $uploadExitCode
