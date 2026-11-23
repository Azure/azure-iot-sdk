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

# templates/variables-e2e-resource-group.yml; teardown deletes it by the same name.
$resourceGroup = $env:E2E_RESOURCE_GROUP
if (-not $resourceGroup) { throw 'E2E_RESOURCE_GROUP is not set.' }
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
if ($env:E2E_SUITE -in @('c-e2e', 'c-samples')) {
    # One enrollment per job (c-e2e: test device index 0..11; c-samples: samples slot 0..7), a
    # symmetric-key group for the SAS suite and samples, and file upload. Local authentication is
    # disabled, so the tests authorize hub service calls with a Microsoft Entra token.
    $arguments.DpsX509IndividualEnrollments = if ($env:E2E_SUITE -eq 'c-e2e') { 12 } else { 8 }
    $arguments.DpsX509GroupEnrollmentDevices = 0
    $arguments.DpsSymmKeyGroupEnrollmentDevices = 1
    $arguments.EnableFileUpload = $true
    $arguments.DisableLocalAuth = $true
}
if ($env:E2E_SUITE -in @('c-e2e-csr', 'c-samples-csr')) {
    # A certificate-management (ADR) environment: one bootstrap identity under the X.509 group per
    # job (c-e2e-csr: test_csr device index 0..1; c-samples-csr: CSR samples slot 0..3), and a
    # symmetric-key group, both linked to the signing CA. The C test configuration needs an
    # individual enrollment. The CSR e2e suites make no hub service calls.
    $arguments.EnableCertificateManagement = $true
    $arguments.DpsX509IndividualEnrollments = 1
    $arguments.DpsX509GroupEnrollmentDevices = if ($env:E2E_SUITE -eq 'c-e2e-csr') { 2 } else { 4 }
    $arguments.DpsSymmKeyGroupEnrollmentDevices = 1
    $arguments.DisableLocalAuth = $true
}
# The suites whose tests read device telemetry: one consumer group per job, since Event Hubs allows
# 5 readers per partition per consumer group.
$consumerGroups = @(switch ($env:E2E_SUITE) {
    'c-e2e' { 0..11 | ForEach-Object { "e2e-$_" } }
    'c-samples' { 0..7 | ForEach-Object { "samples-$_" } }
    'c-samples-csr' { 0..3 | ForEach-Object { "samples-$_" } }
})
$testEnvironment = New-AzIotTestEnvironment @arguments

if ($consumerGroups.Count -gt 0) {
    # The connection string names the hub; its key is unusable with local authentication disabled.
    $hostName = ($testEnvironment.IotHub.ConnectionString -split ';' | Where-Object { $_ -like 'HostName=*' }) -replace '^HostName=', ''
    $hubName = "$hostName".Split('.')[0]
    if (-not $hubName) { throw 'Could not read the IoT Hub name from the test environment.' }

    # The built-in Event Hub-compatible endpoint accepts only SAS keys, which a hub without local
    # authentication refuses. Device telemetry is routed instead to an Event Hub with local
    # authentication disabled: the hub sends with its managed identity, the tests read with a
    # Microsoft Entra token. Once Azure Device Registration links a hub, 'az iot hub' refuses to
    # update it, so that routing is written through ARM, at the module's IoT Hub api-version.
    $route = @{
        ResourceGroup = $resourceGroup
        IotHubName = $hubName
        Location = $env:E2E_LOCATION
        ConsumerGroups = $consumerGroups
    }
    if ($arguments.EnableCertificateManagement) {
        $route.IotHubApiVersion = & (Get-Module AzIotSdkTest) { $script:IotHubApiVersion }
    }
    # The configuration renderer then writes this Event Hub, with no key, as the telemetry source.
    $testEnvironment.IotHub.TelemetryEventHub = Add-AzIotEventHubTelemetryRoute @route
}

New-Item -ItemType Directory -Force $env:E2E_CONFIG_DIRECTORY | Out-Null
$configPath = Join-Path $env:E2E_CONFIG_DIRECTORY 'set_test_env_vars.ps1'
try {
    New-AzIotCSDKE2ETestConfig -TestEnvInfo $testEnvironment -Target powershell -OutFile $configPath

    if ($consumerGroups.Count -gt 0) {
        # Reads are refused until the Event Hubs role assignments apply; the run scripts wait until then.
        Add-Content $configPath "`$env:E2E_EVENTHUB_READABLE_AFTER = '$($testEnvironment.IotHub.TelemetryEventHub.ReadableAfterUnixSeconds)'"
    }

    # The config renderer omits the enrollment group's intermediate-signed bootstrap identities:
    # each as IOT_DPS_GROUP_X509_*_<i>, and the first also without the suffix.
    if ($testEnvironment.Dps.Enrollments.GroupX509.Count -gt 0) {
        $group = $testEnvironment.Dps.Enrollments.GroupX509[0]
        for ($i = 0; $i -lt $group.Identities.Count; $i++) {
            $identity = $group.Identities[$i]
            $chain = $identity.Certificate.ToPem() + "`n" + $group.Certificate.ToPem() + "`n" +
                $testEnvironment.Dps.RootCaCertificates[0].ToPem()
            $values = @{
                IOT_DPS_GROUP_X509_REGISTRATION_ID = $identity.Id
                IOT_DPS_GROUP_X509_CERTIFICATE = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($chain))
                IOT_DPS_GROUP_X509_KEY = [Convert]::ToBase64String(
                    [Text.Encoding]::UTF8.GetBytes($identity.Certificate.PrivateKey.ToRsaPkcs1Pem()))
            }
            foreach ($entry in $values.GetEnumerator()) {
                $value = $entry.Value.Replace("'", "''")
                Add-Content $configPath ("`$env:{0}_{1} = '{2}'" -f $entry.Key, $i, $value)
                if ($i -eq 0) { Add-Content $configPath ("`$env:{0} = '{1}'" -f $entry.Key, $value) }
            }
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
