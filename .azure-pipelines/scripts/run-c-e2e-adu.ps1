# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
One step of the C software updates e2e pipeline (ci-c-e2e-adu.yml), on the shared MQTTv5 environment.

.DESCRIPTION
The service side is c/tests/e2e/scripts/SuE2E.psm1. Every step but RunTests runs in an AzureCLI@2
task: it reads the environment from the E2E-MQTTV5-SHARED-* secrets of Key Vault
E2E_SHARED_KEY_VAULT and calls Azure as the service connection's principal.
  StageOffers   stages (or reuses) the standing offers into SU_OFFERS_STATE.
  IssueDevices  issues the leg's four devices (c/eng/e2e-shared-device.ps1) into E2E_SU_DEVICES and
                sets the pipeline variables the later steps read.
  RunTests      runs each adapter's two suites, each on its own device, with JUnit results in
                E2E_RESULTS_DIRECTORY. No Azure call.
  CheckResults  checks what the service recorded for each offer suite's device.
#>
param(
    [Parameter(Mandatory)][ValidateSet('StageOffers', 'IssueDevices', 'RunTests', 'CheckResults')][string]$Step
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../../c/tests/e2e/scripts/SuE2E.psm1')
# Standing offers, staged once per version of the code that defines them.
$runId = "s$(Get-SuE2EFixtureTag)"
# Device name -> its suite and adapter.
$devices = [ordered]@{
    'paho'          = @{ Suite = 'su'; Adapter = 'paho' }
    'paho-offer'    = @{ Suite = 'su_offer'; Adapter = 'paho' }
    'az-mqtt'       = @{ Suite = 'su'; Adapter = 'az_mqtt' }
    'az-mqtt-offer' = @{ Suite = 'su_offer'; Adapter = 'az_mqtt' }
}

function Get-SharedValue([string]$Name, [switch]$Secret) {
    $vaultSecret = "E2E-MQTTV5-SHARED-$Name"
    $value = az keyvault secret show --vault-name $env:E2E_SHARED_KEY_VAULT --name $vaultSecret --query value -o tsv --only-show-errors
    if ($LASTEXITCODE -ne 0 -or -not $value) { throw "Could not read $vaultSecret from $($env:E2E_SHARED_KEY_VAULT)." }
    if ($Secret) { Write-Host "##vso[task.setsecret]$value" }
    return $value
}

# The AZ_IOT_E2E_SU_* configuration SuE2E.psm1 reads.
function Set-SuE2EServiceConfiguration {
    $names = [ordered]@{
        SUBSCRIPTION_ID = 'SUBSCRIPTION-ID'; RESOURCE_GROUP = 'RESOURCE-GROUP'; LOCATION = 'LOCATION'
        ADR_NAMESPACE = 'ADR-NAMESPACE'; ADU_ENDPOINT = 'SU-ENDPOINT'
        STORAGE_ACCOUNT = 'SU-STORAGE-ACCOUNT'; STORAGE_CONTAINER = 'SU-STORAGE-CONTAINER'
    }
    foreach ($name in $names.Keys) { Set-Item "env:AZ_IOT_E2E_SU_$name" (Get-SharedValue $names[$name]) }
}

function Get-DeviceVariable([string]$Device) { 'SU_REG_ID_' + $Device.Replace('-', '_').ToUpperInvariant() }

switch ($Step) {
    'StageOffers' {
        Set-SuE2EServiceConfiguration
        New-Item -ItemType Directory -Force -Path (Split-Path $env:SU_OFFERS_STATE) | Out-Null
        New-SuE2EOffers -RunId $runId -StatePath $env:SU_OFFERS_STATE -Reuse
    }
    'IssueDevices' {
        # GROUP_CA signs devices that the shared enrollment group admits.
        $env:E2E_MQTTV5_SHARED_GROUP_CA = Get-SharedValue GROUP-CA -Secret
        $env:E2E_MQTTV5_SHARED_ID_SCOPE = Get-SharedValue ID-SCOPE
        $env:E2E_MQTTV5_SHARED_DPS_HOST = Get-SharedValue DPS-HOST
        $directory = $env:E2E_SU_DEVICES
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        if (-not $IsWindows) { chmod 700 $directory }
        # A new device per suite execution: an offer reaches a device once, and the offer suite
        # starts from a device with no registration.
        foreach ($device in $devices.Keys) {
            $registrationId = "ci-ado-su-$device-$($env:E2E_RUN_ID)-$($env:E2E_OS_TAG)"
            $config = Join-Path $directory "$device.ps1"
            & (Join-Path $PSScriptRoot '../../c/eng/e2e-shared-device.ps1') -Prefix E2E_MQTTV5_SHARED `
                -DeviceOnly -RegistrationId $registrationId -OutFile $config
            . $config
            $key = Join-Path $directory "$device-key.pem"
            [IO.File]::WriteAllBytes((Join-Path $directory "$device-cert.pem"), [Convert]::FromBase64String($env:IOT_DPS_INDIVIDUAL_X509_CERTIFICATE_0))
            [IO.File]::WriteAllBytes($key, [Convert]::FromBase64String($env:IOT_DPS_INDIVIDUAL_X509_KEY_0))
            Remove-Item $config
            if (-not $IsWindows) { chmod 600 $key }
            Write-Host "##vso[task.setvariable variable=$(Get-DeviceVariable $device)]$registrationId"
        }
        $trustedCa = & (Join-Path $PSScriptRoot '../../c/eng/e2e-trusted-ca.ps1') -Path (Join-Path $directory 'trusted-ca.pem')
        Write-Host "##vso[task.setvariable variable=AZ_IOT_E2E_SU_TRUSTED_CA]$trustedCa"
        Write-Host "##vso[task.setvariable variable=AZ_IOT_E2E_SU_ID_SCOPE]$($env:E2E_MQTTV5_SHARED_ID_SCOPE)"
        Write-Host "##vso[task.setvariable variable=AZ_IOT_E2E_SU_DPS_HOST]$($env:E2E_MQTTV5_SHARED_DPS_HOST)"
    }
    'RunTests' {
        Export-SuE2EOfferVariables -RunId $runId
        New-Item -ItemType Directory -Force -Path $env:E2E_RESULTS_DIRECTORY | Out-Null
        # e2e_su: 10 min. e2e_su_offer: 84.5 min, 11 offers x (90 s check + 300 s workflow), plus
        # the scenarios without an offer and setup.
        $timeouts = @{ su = 600; su_offer = 5070 }
        $failed = @()
        foreach ($device in $devices.Keys) {
            $suite = $devices[$device]
            $test = "az_iot_tests_e2e_$($suite.Suite)$(if ($suite.Adapter -eq 'az_mqtt') { '_az_mqtt' })"
            $env:AZ_IOT_E2E_SU_REG_ID = [Environment]::GetEnvironmentVariable((Get-DeviceVariable $device))
            if (-not $env:AZ_IOT_E2E_SU_REG_ID) { throw "No device $device; run IssueDevices first." }
            $env:AZ_IOT_E2E_SU_CERT = Join-Path $env:E2E_SU_DEVICES "$device-cert.pem"
            $env:AZ_IOT_E2E_SU_KEY = Join-Path $env:E2E_SU_DEVICES "$device-key.pem"
            ctest --test-dir "c/build/$($env:E2E_PRESET)" -C Debug -R "^$test$" --verbose --no-tests=error `
                --timeout $timeouts[$suite.Suite] --output-junit (Join-Path $env:E2E_RESULTS_DIRECTORY "$test-$($env:E2E_OS_TAG).xml")
            if ($LASTEXITCODE -ne 0) { $failed += $test }
        }
        if ($failed) { throw "Failed: $($failed -join ', ')." }
    }
    'CheckResults' {
        Set-SuE2EServiceConfiguration
        # The reference microsoft/delta:1 handler is POSIX only.
        $skip = if ($IsWindows) { @('MICROSOFT_DELTA') } else { @() }
        $failed = @()
        foreach ($device in 'paho-offer', 'az-mqtt-offer') {
            $registrationId = [Environment]::GetEnvironmentVariable((Get-DeviceVariable $device))
            if (-not $registrationId) { throw "No device $device; run IssueDevices first." }
            Write-Host "== $registrationId"
            try { Test-SuE2EOffers -StatePath $env:SU_OFFERS_STATE -RegistrationId $registrationId -Skip $skip }
            catch { Write-Host "##vso[task.logissue type=error]$registrationId`: $_"; $failed += $registrationId }
        }
        if ($failed) { throw "Unexpected job results for $($failed -join ', ')." }
    }
}
