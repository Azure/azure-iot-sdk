# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
Deletes this run's devices from a shared e2e IoT Hub.

.DESCRIPTION
Shared e2e runs register devices through long-lived DPS enrollment groups; each registration
creates a hub device that nothing else removes. Deletes each -DeviceId (missing ones are
fine) with the connection string in E2E_IOTHUB_REGISTRY_CS (a policy with RegistryWrite).
Every failure, including a missing or malformed connection string, is a warning and the
script exits 0, so cleanup never decides a run's result.
#>
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,127}$')][string[]]$DeviceId
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-AuthHeaders([string]$ConnectionString) {
    $Parts = @{}
    foreach ($Pair in $ConnectionString.Split(';', [StringSplitOptions]::RemoveEmptyEntries)) {
        $Name, $Value = $Pair.Split('=', 2)
        $Parts[$Name] = $Value
    }
    foreach ($Name in 'HostName', 'SharedAccessKeyName', 'SharedAccessKey') {
        if (-not $Parts.ContainsKey($Name)) { throw "it has no $Name" }
    }
    $Resource = [uri]::EscapeDataString($Parts.HostName)
    $Expiry = [DateTimeOffset]::UtcNow.AddMinutes(10).ToUnixTimeSeconds()
    $Hmac = [System.Security.Cryptography.HMACSHA256]::new([Convert]::FromBase64String($Parts.SharedAccessKey))
    $Signature = [Convert]::ToBase64String($Hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("$Resource`n$Expiry")))
    return @{
        Host    = $Parts.HostName
        Headers = @{
            Authorization = "SharedAccessSignature sr=$Resource&sig=$([uri]::EscapeDataString($Signature))&se=$Expiry&skn=$($Parts.SharedAccessKeyName)"
            'If-Match'    = '*'
        }
    }
}

if ([string]::IsNullOrWhiteSpace($env:E2E_IOTHUB_REGISTRY_CS)) {
    Write-Host "::warning::E2E_IOTHUB_REGISTRY_CS not set; devices not deleted: $($DeviceId -join ', ')"
    exit 0
}
try {
    $Auth = Get-AuthHeaders $env:E2E_IOTHUB_REGISTRY_CS
} catch {
    # The message names only what is missing or malformed, never the value.
    Write-Host "::warning::E2E_IOTHUB_REGISTRY_CS is unusable ($($_.Exception.Message)); devices not deleted: $($DeviceId -join ', ')"
    exit 0
}

foreach ($Id in $DeviceId) {
    $Uri = "https://$($Auth.Host)/devices/$($Id)?api-version=2021-04-12"
    try {
        Invoke-WebRequest -Method Delete -Uri $Uri -Headers $Auth.Headers -UseBasicParsing | Out-Null
        Write-Host "Deleted device '$Id'."
    } catch {
        $Status = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 0 }
        if ($Status -eq 404) {
            Write-Host "Device '$Id' not found; nothing to delete."
        } else {
            Write-Host "::warning::Could not delete device '$Id' (HTTP $Status): $($_.Exception.Message)"
        }
    }
}
exit 0
