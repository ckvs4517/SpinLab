@echo off
set "PROJECT_ROOT=%~dp0"

REM Open a nested PowerShell in this terminal with ESP-IDF already activated.
powershell.exe -NoExit -ExecutionPolicy Bypass -Command "Set-Location -LiteralPath '%PROJECT_ROOT%'; . 'C:\Espressif\tools\Microsoft.v5.5.2.PowerShell_profile.ps1'; Write-Host 'SpinLab ESP-IDF environment ready.' -ForegroundColor Green"
