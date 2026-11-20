# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Runs one C e2e job (jobs-c-e2e.yml): one device of the provisioned environment and one group of
# suites, then, for the core group on Linux, the PKCS#11 custody suite. The service calls of the
# tests use IOTHUB_SERVICE_ACCESS_TOKEN (Microsoft Entra), since the hub has local auth disabled.

$ErrorActionPreference = 'Stop'

foreach ($name in @('IOTHUB_SERVICE_ACCESS_TOKEN', 'E2E_PRESET', 'E2E_GROUP', 'E2E_DEVICE_INDEX_OS',
                    'E2E_DEVICE_INDEX_GROUP', 'E2E_TESTS', 'E2E_TESTS_EXCLUDE', 'E2E_RUN_ID',
                    'E2E_RESULTS_DIRECTORY')) {
    if (-not (Get-Item "env:$name" -ErrorAction SilentlyContinue).Value) { throw "$name is not set." }
}

& (Join-Path $PSScriptRoot 'import-test-config.ps1')

$index = [int]$env:E2E_DEVICE_INDEX_OS * 4 + [int]$env:E2E_DEVICE_INDEX_GROUP
$registrationId = (Get-Item "env:IOT_DPS_INDIVIDUAL_REGISTRATION_ID_$index" -ErrorAction SilentlyContinue).Value
$certificateBase64 = (Get-Item "env:IOT_DPS_INDIVIDUAL_X509_CERTIFICATE_$index" -ErrorAction SilentlyContinue).Value
$keyBase64 = (Get-Item "env:IOT_DPS_INDIVIDUAL_X509_KEY_$index" -ErrorAction SilentlyContinue).Value
if (-not $registrationId -or -not $certificateBase64 -or -not $keyBase64) {
    throw "The test configuration has no X.509 device $index."
}
if (-not $env:IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY) {
    throw 'The test configuration has no DPS symmetric-key enrollment group (IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY).'
}
Write-Host "e2e device (index $index): $registrationId"

$deviceDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("az-iot-c-e2e-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $deviceDirectory -Force | Out-Null
$certificatePath = Join-Path $deviceDirectory 'device-cert.pem'
$keyPath = Join-Path $deviceDirectory 'device-key.pem'
[System.IO.File]::WriteAllBytes($certificatePath, [Convert]::FromBase64String($certificateBase64))
[System.IO.File]::WriteAllBytes($keyPath, [Convert]::FromBase64String($keyBase64))

$env:AZ_IOT_DPS_ID_SCOPE = $env:IOT_DPS_ID_SCOPE
$env:AZ_IOT_DPS_REGISTRATION_ID = $registrationId
$env:AZ_IOT_CLIENT_CERT = $certificatePath
$env:AZ_IOT_CLIENT_KEY = $keyPath
$env:AZ_IOT_TRUSTED_CA = & ./c/eng/e2e-trusted-ca.ps1 -Path (Join-Path $deviceDirectory 'trusted-ca.pem')
if ($env:IOT_DPS_GLOBAL_ENDPOINT) { $env:AZ_IOT_DPS_GLOBAL_ENDPOINT = $env:IOT_DPS_GLOBAL_ENDPOINT }
$env:AZ_IOT_DPS_SAS_GROUP_KEY = $env:IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY
$env:AZ_IOT_DPS_SAS_REGISTRATION_ID = "sas-$($env:E2E_RUN_ID)-$index"
# Each job reads events on its own consumer group (provision.ps1).
$env:IOTHUB_EVENTHUB_CONSUMER_GROUP = "e2e-$index"
# Schannel handshake and renegotiation trace of the service harness.
if ($IsWindows) { $env:AZ_AMQP_TLS_DEBUG = '1' }

$buildDirectory = "c/build/$($env:E2E_PRESET)"
$configuration = if ($IsWindows) { @('-C', 'Debug') } else { @() }
$resultsPrefix = Join-Path $env:E2E_RESULTS_DIRECTORY "c-e2e-$($env:AGENT_OS)-$($env:E2E_OPENSSL)-$($env:E2E_GROUP)"
New-Item -ItemType Directory -Path $env:E2E_RESULTS_DIRECTORY -Force | Out-Null

ctest --test-dir $buildDirectory @configuration -R $env:E2E_TESTS -E $env:E2E_TESTS_EXCLUDE `
    --verbose --no-tests=error --timeout 900 --output-junit "$resultsPrefix.xml"
$failed = $LASTEXITCODE -ne 0
if ($failed) { Write-Host "##vso[task.logissue type=error]$($env:E2E_GROUP) e2e suites failed ($LASTEXITCODE)." }

# Storage / custody (docs/eng/certificate-management.md): the same device, with its private key in
# a SoftHSM2 token. --keep-key-file: the suites above read the key by path.
if ($IsLinux -and $env:E2E_GROUP -eq 'core') {
    # pkcs11-provider 0.6 (the script's default) does not build on OpenSSL 4.x.
    if ($env:E2E_OPENSSL -ne 'system') { $env:PKCS11_PROVIDER_TAG = 'v1.3.0' }
    # Evaluated, so setup-softhsm.sh sees OPENSSL_MODULES and names the module in its configuration.
    $exports = & bash -c 'set -e; mods="$(c/eng/setup-pkcs11-provider.sh)"; eval "$mods"; echo "$mods"; c/eng/setup-softhsm.sh "$AZ_IOT_CLIENT_KEY" --keep-key-file'
    if ($LASTEXITCODE -ne 0) { throw "PKCS#11 token provisioning failed ($LASTEXITCODE)." }
    foreach ($line in $exports) {
        if ($line -match "^export ([A-Za-z0-9_]+)='(.*)'$") {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
        }
    }
    Write-Host "--- OPENSSL_CONF = $env:OPENSSL_CONF"
    Get-Content $env:OPENSSL_CONF | Write-Host

    # The provider must be active from the configuration alone, which -provider pkcs11 would mask.
    $providers = & openssl list -providers 2>&1 | Out-String
    Write-Host "--- providers active from configuration:`n$providers"
    if ($LASTEXITCODE -ne 0) { throw "openssl list -providers failed ($LASTEXITCODE)." }
    if ($providers -notmatch '(?m)^\s+pkcs11\s*$') {
        throw 'The pkcs11 provider is not active from OPENSSL_CONF.'
    }

    $env:AZ_IOT_PAHO_TRACE = 'error'
    $env:AZ_IOT_E2E_LOG_LEVEL = 'DEBUG'
    ctest --test-dir $buildDirectory -R e2e_custody --verbose --no-tests=error --timeout 900 `
        --output-junit "$resultsPrefix-custody.xml"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "##vso[task.logissue type=error]Custody e2e failed ($LASTEXITCODE)."
        $failed = $true
    }
}

if ($failed) { exit 1 }
