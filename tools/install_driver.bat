@echo off
setlocal

:: Garante que o diretorio atual seja o mesmo do script
cd /d "%~dp0"

:: Executa o script PowerShell com política de bypass
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_driver.ps1"
