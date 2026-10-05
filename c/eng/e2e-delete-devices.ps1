# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
Deletes this run's devices from a shared e2e IoT Hub.

.DESCRIPTION
Shared e2e runs register devices through long-lived DPS enrollment groups; each registration
creates a hub device that nothing else removes. Deletes each -DeviceId (missing ones are
fine) with the connection string in E2E_IOTHUB_REGISTRY_CS (a policy with RegistryWrite).
Failures are reported as warnings, so cleanup never decides a run's result.
#>
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,127}$')][string[]]$DeviceId
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Cs = $env:E2E_IOTHUB_REGISTRY_CS
if ([string]::IsNullOrWhiteSpace($Cs)) {
    Write-Host "::warning::E2E_IOTHUB_REGISTRY_CS not set; devices not deleted: $($DeviceId -join ', ')"
    exit 0
}
$Parts = @{}
foreach ($Pair in $Cs.Split(';', [StringSplitOptions]::RemoveEmptyEntries)) {
    $Name, $Value = $Pair.Split('=', 2)
    $Parts[$Name] = $Value
}
foreach ($Name in 'HostName', 'SharedAccessKeyName', 'SharedAccessKey') {
    if (-not $Parts.ContainsKey($Name)) {
        Write-Host "::warning::E2E_IOTHUB_REGISTRY_CS has no $Name; devices not deleted: $($DeviceId -join ', ')"
        exit 0
    }
}

$Resource = [uri]::EscapeDataString($Parts.HostName)
$Expiry = [DateTimeOffset]::UtcNow.AddMinutes(10).ToUnixTimeSeconds()
$Hmac = [System.Security.Cryptography.HMACSHA256]::new([Convert]::FromBase64String($Parts.SharedAccessKey))
$Signature = [Convert]::ToBase64String($Hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("$Resource`n$Expiry")))
$Headers = @{
    Authorization = "SharedAccessSignature sr=$Resource&sig=$([uri]::EscapeDataString($Signature))&se=$Expiry&skn=$($Parts.SharedAccessKeyName)"
    'If-Match'    = '*'
}

foreach ($Id in $DeviceId) {
    $Uri = "https://$($Parts.HostName)/devices/$($Id)?api-version=2021-04-12"
    try {
        Invoke-WebRequest -Method Delete -Uri $Uri -Headers $Headers -UseBasicParsing | Out-Null
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
