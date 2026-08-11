<#!
.SYNOPSIS
    SpinLab ESP-IDF command helper.

.EXAMPLE
    .\go.ps1 rebuild
    .\go.ps1 clean
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('rebuild', 'clean')]
    [string]$Command
)

$ErrorActionPreference = 'Stop'
$idfActivationScript = 'C:\Espressif\tools\Microsoft.v5.5.2.PowerShell_profile.ps1'

if (-not (Test-Path -LiteralPath $idfActivationScript)) {
    throw "ESP-IDF activation script was not found: $idfActivationScript"
}

# This change applies only to the PowerShell process running this script.
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. $idfActivationScript

switch ($Command) {
    'clean' {
        Write-Host 'Cleaning ESP-IDF build output...' -ForegroundColor Cyan
        idf.py fullclean
    }
    'rebuild' {
        Write-Host 'Cleaning ESP-IDF build output...' -ForegroundColor Cyan
        idf.py fullclean
        Write-Host 'Building SpinLab firmware...' -ForegroundColor Cyan
        idf.py build
    }
}
