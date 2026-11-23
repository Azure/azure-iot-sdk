# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Runs in the AzureCLI@2 task of e2e-mqttv5-adr-namespace.yml: runs
# c/eng/e2e-mqttv5-adr-namespace.ps1 on the shared MQTTv5 e2e environment.
#
# Reads from Key Vault E2E_SHARED_KEY_VAULT the secrets E2E-MQTTV5-SHARED-RESOURCE-GROUP, -HUB-NAME,
# -DPS-NAME and -ADR-NAMESPACE, and the optional -SUBSCRIPTION-ID (default: the service connection's
# subscription), -ADR-API-VERSION (default: the script's) and -SU-INSTANCE (Software Updates
# instance kept linked as updating/su-1).
# The service connection's principal needs Key Vault Secrets User on the vault and Contributor on
# the resource group. Grant the link's role assignments on the resource group once; otherwise it
# also needs role assignment write.
#
# A repair can take hours, and two must not overlap: this exits when any other run of the same
# pipeline is in progress (SYSTEM_ACCESSTOKEN reads the pipeline's builds). Build ids do not give
# the order runs execute in (a retried run keeps its id), so two runs starting together both exit
# and the next scheduled run repairs.

$ErrorActionPreference = 'Stop'

$builds = Invoke-RestMethod -Headers @{ Authorization = "Bearer $env:SYSTEM_ACCESSTOKEN" } -Uri (
    "$($env:SYSTEM_COLLECTIONURI)$($env:SYSTEM_TEAMPROJECTID)/_apis/build/builds" +
    "?definitions=$($env:SYSTEM_DEFINITIONID)&statusFilter=inProgress&api-version=7.1")
$running = @($builds.value | Where-Object { $_.id -ne [long]$env:BUILD_BUILDID })
if ($running.Count -gt 0) {
    Write-Host "Run $($running[0].id) is still ensuring the namespace; nothing to do."
    return
}

function Get-SharedValue([string]$Name, [switch]$Optional) {
    $secret = "E2E-MQTTV5-SHARED-$Name"
    $value = if ($Optional) {
        az keyvault secret show --vault-name $env:E2E_SHARED_KEY_VAULT --name $secret --query value -o tsv --only-show-errors 2>$null
    } else {
        az keyvault secret show --vault-name $env:E2E_SHARED_KEY_VAULT --name $secret --query value -o tsv --only-show-errors
    }
    if ($LASTEXITCODE -eq 0 -and $value) { return $value.Trim() }
    if (-not $Optional) { throw "Could not read $secret from $($env:E2E_SHARED_KEY_VAULT)." }
}

$subscriptionId = Get-SharedValue SUBSCRIPTION-ID -Optional
if (-not $subscriptionId) {
    $subscriptionId = az account show --query id -o tsv --only-show-errors
    if ($LASTEXITCODE -ne 0 -or -not $subscriptionId) { throw 'Could not read the service connection subscription.' }
}
$arguments = @{
    SubscriptionId = $subscriptionId
    ResourceGroup  = Get-SharedValue RESOURCE-GROUP
    HubName        = Get-SharedValue HUB-NAME
    DpsName        = Get-SharedValue DPS-NAME
    NamespaceName  = Get-SharedValue ADR-NAMESPACE
}
$adrApiVersion = Get-SharedValue ADR-API-VERSION -Optional
if ($adrApiVersion) { $arguments.AdrApiVersion = $adrApiVersion }
$updateInstance = Get-SharedValue SU-INSTANCE -Optional
if ($updateInstance) { $arguments.UpdateInstanceName = $updateInstance }

& (Join-Path $PSScriptRoot '../../c/eng/e2e-mqttv5-adr-namespace.ps1') @arguments
