# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Runs in an AzureCLI@2 task of a C e2e job (jobs-c-e2e.yml). Sets the secret variables
# hubAccessToken and eventHubsAccessToken: the service connection's Microsoft Entra tokens for
# https://iothubs.azure.net and https://eventhubs.azure.net. Not with E2E_CSR: the CSR suites make
# no hub service calls.
#
# With E2E_SHARED_KEY_VAULT, the job runs on the shared environment: this reads its E2E-SHARED-*
# (with E2E_CSR, E2E-CSR-SHARED-*) secrets from that vault and writes the job's test configuration
# (c/eng/e2e-shared-device.ps1 -EntraAuthentication, or -Csr) to
# E2E_CONFIG_DIRECTORY/set_test_env_vars.ps1, with a new device at index
# E2E_DEVICE_INDEX_OS * 4 + E2E_DEVICE_INDEX_GROUP or, for a samples job, at index E2E_SAMPLES_SLOT
# with consumer group samples-<slot>. The configuration holds keys: import-test-config.ps1 reads and
# deletes it.

$ErrorActionPreference = 'Stop'
$csr = $env:E2E_CSR -eq 'true'

if (-not $csr) {
    foreach ($resource in @{ hubAccessToken = 'https://iothubs.azure.net'; eventHubsAccessToken = 'https://eventhubs.azure.net' }.GetEnumerator()) {
        $token = az account get-access-token --resource $resource.Value --query accessToken -o tsv --only-show-errors
        if ($LASTEXITCODE -ne 0 -or -not $token) { throw "Could not get an access token for $($resource.Value)." }
        Write-Host "##vso[task.setvariable variable=$($resource.Key);issecret=true]$token"
    }
}

if (-not $env:E2E_SHARED_KEY_VAULT) { return }

$samples = [bool]$env:E2E_SAMPLES_SLOT
if ($samples -and $csr) { throw 'E2E_SAMPLES_SLOT and E2E_CSR cannot be combined.' }
$required = @('E2E_RUN_ID', 'E2E_CONFIG_DIRECTORY', 'BUILD_BUILDID')
if (-not $samples) { $required += @('E2E_DEVICE_INDEX_OS', 'E2E_DEVICE_INDEX_GROUP') }
foreach ($name in $required) {
    if (-not (Get-Item "env:$name" -ErrorAction SilentlyContinue).Value) { throw "$name is not set." }
}

$prefix = if ($csr) { 'E2E_CSR_SHARED' } else { 'E2E_SHARED' }
$names = if ($csr) { @('GROUP_CA', 'ID_SCOPE', 'DPS_HOST', 'SAS_GROUP_KEY') }
         else { @('GROUP_CA', 'IOTHUB_HOSTNAME', 'EVENTHUB_KEYLESS_CS', 'EVENTHUB_PARTITION_COUNT', 'ID_SCOPE', 'SAS_GROUP_KEY', 'DPS_CS') }
# Masked: the CA key signs devices that the shared enrollment group admits.
$secretNames = @('GROUP_CA', 'SAS_GROUP_KEY', 'DPS_CS')
foreach ($name in $names) {
    $secret = "$($prefix)_$name".Replace('_', '-')
    $value = az keyvault secret show --vault-name $env:E2E_SHARED_KEY_VAULT --name $secret --query value -o tsv --only-show-errors
    if ($LASTEXITCODE -ne 0 -or -not $value) { throw "Could not read $secret from $($env:E2E_SHARED_KEY_VAULT)." }
    if ($name -in $secretNames) { Write-Host "##vso[task.setsecret]$value" }
    Set-Item "env:$($prefix)_$name" $value
}

$outFile = Join-Path $env:E2E_CONFIG_DIRECTORY 'set_test_env_vars.ps1'
if ($samples) {
    # Consumer groups samples-0..samples-7 of the shared Event Hub, one per samples job.
    $slot = [int]$env:E2E_SAMPLES_SLOT
    & (Join-Path $PSScriptRoot '../../c/eng/e2e-shared-device.ps1') -EntraAuthentication `
        -RegistrationId "ci-ado-samples-$($env:E2E_RUN_ID)-$slot" -DeviceIndex $slot -ConsumerGroup "samples-$slot" `
        -OutFile $outFile
    return
}
$index = [int]$env:E2E_DEVICE_INDEX_OS * 4 + [int]$env:E2E_DEVICE_INDEX_GROUP
if ($csr) {
    & (Join-Path $PSScriptRoot '../../c/eng/e2e-shared-device.ps1') -Csr `
        -RegistrationId "ci-ado-csr-$($env:E2E_RUN_ID)-$index" -DeviceIndex $index -OutFile $outFile
    return
}
# Consumer groups e2e-0..e2e-11 of the shared Event Hub. Event Hubs allows 5 readers per partition
# and consumer group, and each job reads every partition: a run's 15 jobs use each group at most
# twice, and consecutive runs start at different groups.
$consumerGroup = 'e2e-' + (([long]$env:BUILD_BUILDID + $index) % 12)
& (Join-Path $PSScriptRoot '../../c/eng/e2e-shared-device.ps1') -EntraAuthentication `
    -RegistrationId "ci-ado-$($env:E2E_RUN_ID)-$index" -DeviceIndex $index -ConsumerGroup $consumerGroup `
    -OutFile $outFile
