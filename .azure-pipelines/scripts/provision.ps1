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

$prefix = switch ($env:E2E_SUITE) {
    'standard' { 'DotnetE2ETestPipeline' }
    'csr' { 'DotnetCertMgmtE2ETestPipeline' }
    'longhaul' { 'DotnetLonghaul' }
    default { throw 'Unknown test suite.' }
}
Import-Module (Join-Path $env:E2E_FRAMEWORK_ROOT 'scripts/AzIotSdkTest/AzIotSdkTest.psd1')

$arguments = @{
    ResourceGroup = "$prefix-Ado-$env:E2E_RUN_ID"
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
$testEnvironment = New-AzIotTestEnvironment @arguments

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
    $key = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
    $config = ConvertTo-SecureString (Get-Content $configPath -Raw) -AsPlainText -Force
    ConvertFrom-SecureString $config -Key $key |
        Set-Content (Join-Path $env:E2E_CONFIG_DIRECTORY 'test-config.encrypted')
    $encodedKey = [Convert]::ToBase64String($key)
    Write-Host "##vso[task.setvariable variable=configKey;isOutput=true;issecret=true]$encodedKey"
}
finally {
    Remove-Item $configPath -Force -ErrorAction SilentlyContinue
}
