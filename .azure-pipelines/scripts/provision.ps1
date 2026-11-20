# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

$ErrorActionPreference = 'Stop'

if ($env:E2E_SUITE -eq 'longhaul') {
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $duration = [double]::Parse($env:LONGHAUL_DURATION_MINUTES, $culture)
    $success = [double]::Parse($env:LONGHAUL_MIN_SUCCESS_PERCENT, $culture)
    $growth = [double]::Parse($env:LONGHAUL_MAX_MANAGED_MEMORY_GROWTH_MB, $culture)
    if (-not ($duration -ge 1 -and $duration -le 320 -and $duration -eq [Math]::Floor($duration))) {
        throw 'durationMinutes must be a whole number from 1 to 320.'
    }
    if (-not ($success -gt 0 -and $success -le 100)) {
        throw 'minSuccessPercent must be greater than 0 and at most 100.'
    }
    if (-not ($growth -gt 0 -and -not [double]::IsInfinity($growth))) {
        throw 'maxMemoryGrowthMb must be a finite positive number.'
    }
}

$resourceGroup = & (Join-Path $PSScriptRoot 'e2e-resource-group.ps1') -Suite $env:E2E_SUITE -RunId $env:E2E_RUN_ID
Import-Module (Join-Path $env:E2E_FRAMEWORK_ROOT 'scripts/AzIotSdkTest/AzIotSdkTest.psd1')

$arguments = @{
    ResourceGroup = $resourceGroup
    AzureLocation = $env:E2E_LOCATION
    IotHubX509ThumbprintDevices = 1
    DpsX509IndividualEnrollments = 2
    DpsX509GroupEnrollmentDevices = 2
}
if ($env:E2E_SUITE -eq 'longhaul') {
    $arguments.DpsX509IndividualEnrollments = 1
    $arguments.DpsX509GroupEnrollmentDevices = 0
}
if ($env:E2E_SUITE -eq 'csr') {
    $arguments.EnableCertificateManagement = $true
    $arguments.DpsSymmKeyGroupEnrollmentDevices = 1
}
if ($env:E2E_SUITE -eq 'c-e2e') {
    # One enrollment and Event Hubs consumer group per test job (device index 0..11), a
    # symmetric-key group for the SAS suite, and file upload. Local authentication is disabled,
    # so the tests authorize hub service calls with a Microsoft Entra token.
    $arguments.DpsX509IndividualEnrollments = 12
    $arguments.DpsX509GroupEnrollmentDevices = 0
    $arguments.DpsSymmKeyGroupEnrollmentDevices = 1
    $arguments.EnableFileUpload = $true
    $arguments.DisableLocalAuth = $true
}
$testEnvironment = New-AzIotTestEnvironment @arguments

if ($env:E2E_SUITE -eq 'c-e2e') {
    # The connection string names the hub; its key is unusable with local authentication disabled.
    $hostName = ($testEnvironment.IotHub.ConnectionString -split ';' | Where-Object { $_ -like 'HostName=*' }) -replace '^HostName=', ''
    $hubName = "$hostName".Split('.')[0]
    if (-not $hubName) { throw 'Could not read the IoT Hub name from the test environment.' }
    # The framework leaves file-upload notifications off; the file-upload suite needs them.
    az iot hub update --name $hubName --resource-group $resourceGroup --fileupload-notifications true --only-show-errors | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not enable file-upload notifications on $hubName." }
    # Event Hubs allows 5 readers per partition per consumer group.
    foreach ($index in 0..11) {
        az iot hub consumer-group create --hub-name $hubName --resource-group $resourceGroup --name "e2e-$index" --only-show-errors | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not create consumer group e2e-$index on $hubName." }
    }
}

New-Item -ItemType Directory -Force $env:E2E_CONFIG_DIRECTORY | Out-Null
$configPath = Join-Path $env:E2E_CONFIG_DIRECTORY 'set_test_env_vars.ps1'
try {
    New-AzIotCSDKE2ETestConfig -TestEnvInfo $testEnvironment -Target powershell -OutFile $configPath

    # The config renderer omits the enrollment group's intermediate-signed bootstrap identity.
    if ($testEnvironment.Dps.Enrollments.GroupX509.Count -gt 0) {
        $group = $testEnvironment.Dps.Enrollments.GroupX509[0]
        $identity = $group.Identities[0]
        $chain = $identity.Certificate.ToPem() + "`n" + $group.Certificate.ToPem() + "`n" +
            $testEnvironment.Dps.RootCaCertificates[0].ToPem()
        $values = @{
            IOT_DPS_GROUP_X509_REGISTRATION_ID = $identity.Id
            IOT_DPS_GROUP_X509_CERTIFICATE = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($chain))
            IOT_DPS_GROUP_X509_KEY = [Convert]::ToBase64String(
                [Text.Encoding]::UTF8.GetBytes($identity.Certificate.PrivateKey.ToRsaPkcs1Pem()))
        }
        foreach ($entry in $values.GetEnumerator()) {
            Add-Content $configPath ("`$env:{0} = '{1}'" -f $entry.Key, $entry.Value.Replace("'", "''"))
        }
    }

    # Pipeline artifacts have run-level retention, not GitHub's per-artifact one-day TTL.
    # Transport only ciphertext; pass the random AES key as a secret job output.
    # AES-GCM, not SecureString: ConvertTo-SecureString caps input at 64 KiB, and the C config is larger.
    # The file is nonce (12 bytes), tag (16 bytes), then ciphertext.
    $key = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
    $plaintext = [IO.File]::ReadAllBytes($configPath)
    $nonce = [Security.Cryptography.RandomNumberGenerator]::GetBytes(12)
    $tag = [byte[]]::new(16)
    $ciphertext = [byte[]]::new($plaintext.Length)
    $aes = [Security.Cryptography.AesGcm]::new($key, 16)
    try { $aes.Encrypt($nonce, $plaintext, $ciphertext, $tag) } finally { $aes.Dispose() }
    $encrypted = [byte[]]::new(28 + $ciphertext.Length)
    [Array]::Copy($nonce, 0, $encrypted, 0, 12)
    [Array]::Copy($tag, 0, $encrypted, 12, 16)
    [Array]::Copy($ciphertext, 0, $encrypted, 28, $ciphertext.Length)
    [IO.File]::WriteAllBytes((Join-Path $env:E2E_CONFIG_DIRECTORY 'test-config.encrypted'), $encrypted)
    $encodedKey = [Convert]::ToBase64String($key)
    Write-Host "##vso[task.setvariable variable=configKey;isOutput=true;issecret=true]$encodedKey"
}
finally {
    Remove-Item $configPath -Force -ErrorAction SilentlyContinue
}
