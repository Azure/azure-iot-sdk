#Requires -Version 7.2
<#
.SYNOPSIS
    Cleans up build artifacts and client environment files created by New-TestClient.ps1.

.DESCRIPTION
    1. Removes the CMake build directory.
    2. Removes the .client-env.ps1 file.

    Does NOT remove certificates or the mock environment — use Remove-TestEnv.ps1 for that.

.PARAMETER KeepBuild
    If set, skips deleting the build directory (only removes env files).

.EXAMPLE
    .\Remove-TestClient.ps1
    .\Remove-TestClient.ps1 -KeepBuild
#>
[CmdletBinding()]
param(
    [switch]$KeepBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
$BuildDir  = Join-Path $RepoRoot "build"
$EnvFile   = Join-Path $ScriptDir ".client-env.ps1"

Write-Host "=== Remove Test Client Artifacts ===" -ForegroundColor Cyan

# ─────────────────────────────────────────────────────────────────────────────
# Remove env file
# ─────────────────────────────────────────────────────────────────────────────

if (Test-Path $EnvFile) {
    Write-Host "[env] Removing: $EnvFile"
    Remove-Item $EnvFile -Force
}

# ─────────────────────────────────────────────────────────────────────────────
# Remove build directory
# ─────────────────────────────────────────────────────────────────────────────

if (-not $KeepBuild) {
    if (Test-Path $BuildDir) {
        Write-Host "[build] Removing: $BuildDir"
        Remove-Item $BuildDir -Recurse -Force
    } else {
        Write-Host "[build] Build directory does not exist — nothing to remove."
    }
} else {
    Write-Host "[build] Skipped (KeepBuild flag set)."
}

# ─────────────────────────────────────────────────────────────────────────────
# Clear env vars (current session only)
# ─────────────────────────────────────────────────────────────────────────────

$envVars = @(
    "AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT",
    "AZ_IOT_DEVICE_ID",
    "AZ_IOT_CLIENT_CERT",
    "AZ_IOT_CLIENT_KEY",
    "AZ_IOT_TRUSTED_CA",
    "AZ_IOT_DPS_ID_SCOPE",
    "AZ_IOT_DPS_REGISTRATION_ID"
)
foreach ($v in $envVars) {
    [System.Environment]::SetEnvironmentVariable($v, $null)
}

Write-Host ""
Write-Host "Client artifacts removed." -ForegroundColor Green
