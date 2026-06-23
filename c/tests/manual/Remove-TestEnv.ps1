#Requires -Version 7.2
<#
.SYNOPSIS
    Tears down the IoT-Hub-Next mock environment.

.DESCRIPTION
    1. Stops and removes the Mosquitto container.
    2. Removes the Docker image and volume.
    3. Deletes generated certificates.

.EXAMPLE
    .\Remove-TestEnv.ps1
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir  = $PSScriptRoot
$CertsDir   = Join-Path $ScriptDir ".certs"
$ComposeDir = Join-Path $ScriptDir ".env-hub-next"

Write-Host "=== IoT Hub-Next Mock Environment Teardown ===" -ForegroundColor Cyan

# ─────────────────────────────────────────────────────────────────────────────
# Docker teardown
# ─────────────────────────────────────────────────────────────────────────────

if (Test-Path (Join-Path $ComposeDir "docker-compose.yml")) {
    Push-Location $ComposeDir
    try {
        Write-Host "[docker] Stopping containers and removing images/volumes..."
        docker compose down --rmi all -v 2>&1 | Write-Host
    } finally { Pop-Location }
} else {
    Write-Host "[docker] No docker-compose.yml found — nothing to stop."
}

# ─────────────────────────────────────────────────────────────────────────────
# Remove generated files
# ─────────────────────────────────────────────────────────────────────────────

if (Test-Path $CertsDir) {
    Write-Host "[certs] Removing: $CertsDir"
    Remove-Item $CertsDir -Recurse -Force
}

if (Test-Path $ComposeDir) {
    Write-Host "[config] Removing: $ComposeDir"
    Remove-Item $ComposeDir -Recurse -Force
}

Write-Host ""
Write-Host "Environment removed." -ForegroundColor Green
