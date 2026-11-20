# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

#Requires -Version 7.0
# ARM deployment follows eng/scripts/New-AzureIoTGen2TestEnvironment.ps1 in
# azure-iot-sdks/azure-iot-sdk at da20156ed6c1768a89148b2e090431543d3c2a33.
# Tests create their own individual X.509 enrollments; no shared CA is needed.
$ErrorActionPreference = 'Stop'
$arm = 'https://management.azure.com'
$hubApi = '2026-10-01-preview'
$dpsApi = '2026-11-02-preview'
$adrApi = '2026-11-02-preview'

function Protect-Secrets([string]$Text) {
    $Text = $Text -replace '(?i)("(?:primaryKey|secondaryKey|accessToken)"\s*:\s*")[^"]*', '$1<redacted>'
    $Text = $Text -replace '(?i)(SharedAccessKey\s*=\s*)[^;"\s]+', '$1<redacted>'
    $Text -replace '(?i)(\bsig=)[^&"\s]+', '$1<redacted>'
}

function Invoke-AzureJson([string[]]$Arguments, [switch]$Sensitive) {
    $output = ((& az @Arguments 2>&1) | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
        $detail = if ($Sensitive) { 'response withheld' } else { Protect-Secrets $output }
        throw "Azure command '$($Arguments[0..1] -join ' ')' failed (exit $LASTEXITCODE): $detail"
    }
    if ($output) { $output | ConvertFrom-Json -Depth 50 }
}

function Invoke-Arm([string]$Method, [string]$Url, $Body, [switch]$Sensitive) {
    $arguments = @('rest', '--method', $Method, '--url', $Url, '--only-show-errors', '-o', 'json')
    $file = $null
    try {
        if ($null -ne $Body) {
            $file = New-TemporaryFile
            $Body | ConvertTo-Json -Depth 20 -Compress | Set-Content -LiteralPath $file -Encoding utf8NoBOM
            $arguments += @('--headers', 'Content-Type=application/json', '--body', "@$file")
        }
        Invoke-AzureJson $arguments -Sensitive:$Sensitive
    }
    finally {
        if ($file) { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
    }
}

function Wait-Resource([string]$Url, [string]$Name, [int]$Minutes) {
    $deadline = [DateTime]::UtcNow.AddMinutes($Minutes)
    do {
        $resource = Invoke-Arm get $Url -Sensitive
        $state = $resource.properties.provisioningState
        if ($state -in 'Failed', 'Canceled') { throw "$Name provisioning $state." }
        if ($state -eq 'Succeeded' -and $resource.properties.state -eq 'Active') { return $resource }
        if ([DateTime]::UtcNow -ge $deadline) { throw "$Name did not become active within $Minutes minutes." }
        Write-Host "Waiting for $Name ($state / $($resource.properties.state))."
        Start-Sleep -Seconds 20
    } while ($true)
}

foreach ($name in 'E2E_LOCATION', 'E2E_RUN_ID', 'E2E_CONFIG_DIRECTORY') {
    if (-not [Environment]::GetEnvironmentVariable($name)) { throw "Missing $name." }
}
if ($env:E2E_RUN_ID -notmatch '^[a-fA-F0-9-]{36}-[0-9]+$') { throw 'Invalid E2E_RUN_ID.' }
$account = Invoke-AzureJson @('account', 'show', '--only-show-errors', '-o', 'json')
$subscription = $account.id
$devices = Invoke-AzureJson @('provider', 'show', '--namespace', 'Microsoft.Devices', '--only-show-errors', '-o', 'json')
$registry = Invoke-AzureJson @('provider', 'show', '--namespace', 'Microsoft.DeviceRegistry', '--only-show-errors', '-o', 'json')
foreach ($check in @(
        @($devices, 'IotHubs', $hubApi),
        @($devices, 'provisioningServices', $dpsApi),
        @($registry, 'namespaces', $adrApi))) {
    $type = $check[0].resourceTypes | Where-Object resourceType -EQ $check[1]
    if ($check[0].registrationState -ne 'Registered' -or $check[2] -notin $type.apiVersions) {
        throw "$($check[1]) requires a registered provider serving $($check[2]); check subscription preview access."
    }
}

$resourceGroup = "DotnetMqttv5E2E-Ado-$env:E2E_RUN_ID"
$suffix = [Guid]::NewGuid().ToString('N').Substring(0, 16)
$hubName = "dotnet-mqttv5-hub-$suffix"
$dpsName = "dotnet-mqttv5-dps-$suffix"
$namespaceName = "dotnet-mqttv5-adr-$suffix"
$scope = "/subscriptions/$subscription/resourceGroups/$resourceGroup"
$hubUrl = "$arm$scope/providers/Microsoft.Devices/IotHubs/${hubName}?api-version=$hubApi"
$dpsUrl = "$arm$scope/providers/Microsoft.Devices/provisioningServices/${dpsName}?api-version=$dpsApi"
$exists = Invoke-AzureJson @('group', 'exists', '--name', $resourceGroup, '--only-show-errors', '-o', 'json')
if ($exists) { throw "Resource group $resourceGroup already exists; queue a new run instead of retrying setup." }
[void](Invoke-AzureJson @('group', 'create', '--name', $resourceGroup, '--location', $env:E2E_LOCATION,
        '--tags', 'purpose=dotnet-mqttv5-e2e', "CreatedOn=$([DateTime]::UtcNow.ToString('o'))",
        "project=$env:SYSTEM_TEAMPROJECTID", '--only-show-errors', '-o', 'json'))

# DPS starts first to fail early on subscription quota and provision alongside the hub.
[void](Invoke-Arm put $dpsUrl @{
        location = $env:E2E_LOCATION
        sku = @{ name = 'S1'; capacity = 1 }
        identity = @{ type = 'SystemAssigned' }
        properties = @{}
    })
[void](Invoke-Arm put $hubUrl @{
        location = $env:E2E_LOCATION
        sku = @{ name = 'S1'; capacity = 1 }
        identity = @{ type = 'SystemAssigned' }
        properties = @{
            connectionProfile = 'MqttV5'
            routing = @{ routes = @(@{
                        name = 'device-messages-to-events'; source = 'DeviceMessages'; condition = 'true'
                        endpointNames = @('events'); isEnabled = $true
                    }) }
            mqttV5Settings = @{ topicGroups = @(@{
                        topicGroupId = 'e2e'; topicTemplates = @('e2e/{deviceId}/#', 'e2e/shared/#')
                    }) }
        }
    })
$hub = Wait-Resource $hubUrl 'MQTTv5 hub' 30
[void](Wait-Resource $dpsUrl 'DPS' 15)
if ($hub.properties.connectionProfile -ine 'MqttV5') { throw 'Created hub is not MQTTv5.' }

# The existing helper also creates fresh namespaces and handles RBAC replication.
$linkScript = Join-Path $PSScriptRoot '../../c/eng/e2e-mqttv5-adr-namespace.ps1'
& $linkScript -SubscriptionId $subscription -ResourceGroup $resourceGroup -HubName $hubName `
    -DpsName $dpsName -NamespaceName $namespaceName -HubApiVersion $hubApi `
    -DpsApiVersion $dpsApi -AdrApiVersion $adrApi

$hub = Invoke-Arm get $hubUrl -Sensitive
$dps = Invoke-Arm get $dpsUrl -Sensitive
$group = @($hub.properties.mqttV5Settings.topicGroups | Where-Object topicGroupId -EQ 'e2e')
if ($group.Count -ne 1 -or
    (Compare-Object @($group[0].topicTemplates) @('e2e/{deviceId}/#', 'e2e/shared/#'))) {
    throw 'Hub does not report the required custom-topic templates after ADR linking.'
}
$hubKeys = Invoke-Arm post "$arm$scope/providers/Microsoft.Devices/IotHubs/$hubName/listkeys?api-version=$hubApi" -Sensitive
$dpsKeys = Invoke-Arm post "$arm$scope/providers/Microsoft.Devices/provisioningServices/$dpsName/listkeys?api-version=$dpsApi" -Sensitive
$hubKey = ($hubKeys.value | Where-Object keyName -EQ 'iothubowner').primaryKey
$dpsKey = ($dpsKeys.value | Where-Object keyName -EQ 'provisioningserviceowner').primaryKey
if (-not $hubKey -or -not $dpsKey -or -not $hub.properties.serviceHostName -or
    -not $dps.properties.serviceOperationsHostName -or -not $dps.properties.deviceProvisioningHostName -or
    -not $dps.properties.idScope) {
    throw 'Provisioned environment is missing service credentials or endpoints.'
}
$config = @{
    IOTHUB_CONNECTION_STRING_MQTTV5 = "HostName=$($hub.properties.serviceHostName);SharedAccessKeyName=iothubowner;SharedAccessKey=$hubKey"
    IOT_DPS_CONNECTION_STRING_MQTTV5 = "HostName=$($dps.properties.serviceOperationsHostName);SharedAccessKeyName=provisioningserviceowner;SharedAccessKey=$dpsKey"
    IOT_DPS_ID_SCOPE_MQTTV5 = $dps.properties.idScope
    IOT_DPS_GLOBAL_ENDPOINT_MQTTV5 = $dps.properties.deviceProvisioningHostName
}
$key = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
$secure = ConvertTo-SecureString ($config | ConvertTo-Json -Compress) -AsPlainText -Force
New-Item -ItemType Directory -Force $env:E2E_CONFIG_DIRECTORY | Out-Null
ConvertFrom-SecureString $secure -Key $key |
    Set-Content (Join-Path $env:E2E_CONFIG_DIRECTORY 'test-config.encrypted')
Write-Host "##vso[task.setvariable variable=configKey;isOutput=true;issecret=true]$([Convert]::ToBase64String($key))"
