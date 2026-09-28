#Requires -Version 7.2
<#
.SYNOPSIS
    Shows logs from the mock MQTTv5 service worker container.

.PARAMETER Follow
    Continuously stream new log output (like tail -f).

.EXAMPLE
    .\Read-ServiceLogs.ps1
    .\Read-ServiceLogs.ps1 -Follow
#>
[CmdletBinding()]
param(
    [switch]$Follow
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ComposeFile = Join-Path $PSScriptRoot ".env-mqttv5\docker-compose.yml"

if (-not (Test-Path $ComposeFile)) {
    throw "Environment not found. Run .\New-TestEnv.ps1 first."
}

$args_ = @("compose", "-f", $ComposeFile, "logs", "service")
if ($Follow) { $args_ += "-f" }

& docker @args_
