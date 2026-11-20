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
if ($env:E2E_SUITE -eq 'c-e2e') {
    # One enrollment per test job (device index 0..11), a symmetric-key group for the SAS suite,
    # and file upload. Local authentication is disabled, so the tests authorize hub service calls
    # with a Microsoft Entra token.
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
    # The built-in Event Hub-compatible endpoint accepts only SAS keys, which a hub without local
    # authentication refuses. Device telemetry is routed instead to an Event Hub with local
    # authentication disabled: the hub sends with its managed identity, the tests read with a
    # Microsoft Entra token.
    $eventHubsNamespace = 'csdke2e-' + [Guid]::NewGuid().ToString('N').Substring(0, 12)
    $eventHub = 'telemetry'
    az eventhubs namespace create --name $eventHubsNamespace --resource-group $resourceGroup --location $env:E2E_LOCATION `
        --sku Standard --disable-local-auth true --minimum-tls-version 1.2 --only-show-errors | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not create Event Hubs namespace $eventHubsNamespace." }
    $eventHubId = az eventhubs eventhub create --name $eventHub --namespace-name $eventHubsNamespace --resource-group $resourceGroup `
        --partition-count 4 --query id -o tsv --only-show-errors
    if ($LASTEXITCODE -ne 0 -or -not $eventHubId) { throw "Could not create Event Hub $eventHub." }
    # One consumer group per test job (device index 0..11): Event Hubs allows 5 readers per
    # partition per consumer group.
    foreach ($index in 0..11) {
        az eventhubs eventhub consumer-group create --name "e2e-$index" --eventhub-name $eventHub `
            --namespace-name $eventHubsNamespace --resource-group $resourceGroup --only-show-errors | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not create consumer group e2e-$index on $eventHub." }
    }

    $hubPrincipalId = az iot hub identity assign --name $hubName --resource-group $resourceGroup --system-assigned `
        --query principalId -o tsv --only-show-errors
    if ($LASTEXITCODE -ne 0 -or -not $hubPrincipalId) { throw "Could not assign a system identity to $hubName." }
    # The signed-in principal's object id, from its ARM token: no Microsoft Graph permission needed.
    $payload = (az account get-access-token --query accessToken -o tsv --only-show-errors).Split('.')[1].Replace('-', '+').Replace('_', '/')
    $payload = $payload.PadRight($payload.Length + (4 - $payload.Length % 4) % 4, '=')
    $signedInPrincipalId = ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($payload)) | ConvertFrom-Json).oid
    $signedInPrincipalType = if ((az account show --query user.type -o tsv --only-show-errors) -eq 'user') { 'User' } else { 'ServicePrincipal' }
    # Azure Event Hubs Data Sender to the hub, Data Receiver to the tests.
    $grants = @(
        @{ Principal = $hubPrincipalId; Type = 'ServicePrincipal'; Role = '2b629674-e913-4c01-ae53-ef4638d8f975' }
        @{ Principal = $signedInPrincipalId; Type = $signedInPrincipalType; Role = 'a638d3c7-ab3a-418d-83e6-5f17a39d4fde' }
    )
    foreach ($grant in $grants) {
        az role assignment create --assignee-object-id $grant.Principal --assignee-principal-type $grant.Type `
            --role $grant.Role --scope $eventHubId --only-show-errors | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not assign $($grant.Role) on $eventHub to $($grant.Principal)." }
    }
    # Event Hubs role assignments take up to five minutes to apply; run-c-e2e.ps1 waits until then.
    $eventHubReadableAfter = [DateTimeOffset]::UtcNow.AddMinutes(5).ToUnixTimeSeconds()

    # Creating an identity-based endpoint can fail until the hub's new role propagates.
    for ($attempt = 1; ; $attempt++) {
        $output = az iot hub message-endpoint create eventhub --hub-name $hubName --resource-group $resourceGroup --endpoint-name $eventHub `
            --endpoint-uri "sb://$eventHubsNamespace.servicebus.windows.net" --entity-path $eventHub --identity '[system]' `
            --only-show-errors 2>&1
        if ($LASTEXITCODE -eq 0) { break }
        $output = $output | Out-String
        if ($attempt -ge 10) { throw "Could not create the $eventHub routing endpoint on $hubName`: $output" }
        Write-Host "Routing endpoint not created (attempt $attempt): $output Retrying in 30 seconds."
        Start-Sleep -Seconds 30
    }
    az iot hub message-route create --hub-name $hubName --resource-group $resourceGroup --route-name $eventHub `
        --endpoint-name $eventHub --source-type DeviceMessages --enabled true --only-show-errors | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not route device messages to $eventHub on $hubName." }
}

New-Item -ItemType Directory -Force $env:E2E_CONFIG_DIRECTORY | Out-Null
$configPath = Join-Path $env:E2E_CONFIG_DIRECTORY 'set_test_env_vars.ps1'
try {
    New-AzIotCSDKE2ETestConfig -TestEnvInfo $testEnvironment -Target powershell -OutFile $configPath

    if ($env:E2E_SUITE -eq 'c-e2e') {
        # Appended, so these replace the rendered built-in endpoint. No key: the tests read with a
        # Microsoft Entra token (IOTHUB_EVENTHUB_ACCESS_TOKEN).
        Add-Content $configPath @(
            "`$env:IOTHUB_EVENTHUB_CONNECTION_STRING = 'Endpoint=sb://$eventHubsNamespace.servicebus.windows.net/;EntityPath=$eventHub'"
            "`$env:IOTHUB_EVENTHUB_PARTITION_COUNT = '4'"
            "`$env:E2E_EVENTHUB_READABLE_AFTER = '$eventHubReadableAfter'"
        )
    }

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
