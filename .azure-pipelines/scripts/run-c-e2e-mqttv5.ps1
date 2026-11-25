# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
One step of one leg of the C mqttv5 e2e pipeline (ci-c-e2e-mqttv5.yml), on the shared MQTTv5
environment.

.DESCRIPTION
A leg runs suite E2E_SUITE over adapter E2E_ADAPTER (paho, az_mqtt, or both: every test of the
suite) and shard E2E_SHARD ("<index>/<count>", AZ_IOT_TEST_SHARD; "0/1" runs every test).
  Build        installs the Linux packages the suite needs and builds its tests. No Azure call.
  IssueDevice  runs in an AzureCLI@2 task: reads the E2E-MQTTV5-SHARED-* secrets of Key Vault
               E2E_SHARED_KEY_VAULT, issues the leg's device (c/eng/e2e-shared-device.ps1) and
               writes its test config, certificate and key into E2E_DEVICE_DIRECTORY.
  RunTests     runs the suite with that device, with JUnit results in E2E_RESULTS_DIRECTORY; for
               transport, through a CONNECT proxy on loopback; for custody, with the key moved
               into a SoftHSM2 token. No Azure call.
#>
param(
    [Parameter(Mandatory)][ValidateSet('Build', 'IssueDevice', 'RunTests')][string]$Step
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path

# Per suite: registration ID tag; consumer group slot (group e2e-((build id + slot) mod 11)), plus
# adapter index x shard count + shard index when SpreadGroups, so each leg reads through its own
# group (custody has slot 10 to itself; Event Hubs allows 5 readers per partition and group);
# log level and other test environment.
$suites = @{
    custom_topic  = @{
        Tag = 'ct'; Slot = 0; SpreadGroups = $true; LogLevel = 'TRACE'
        # The hub acknowledges custom topic publishes but does not deliver them to the events
        # endpoint yet; the routed cases skip until it does. The Paho protocol trace shows each
        # DPS response.
        Environment = @{ AZ_IOT_E2E_SKIP_CUSTOM_TOPIC_ROUTING = '1'; AZ_IOT_PAHO_TRACE = 'protocol' }
    }
    twin          = @{ Tag = 'tw'; Slot = 0; LogLevel = 'INFO' }
    connection    = @{ Tag = 'cn'; Slot = 9; LogLevel = 'INFO' }
    telemetry     = @{ Tag = 'te'; Slot = 8; LogLevel = 'INFO' }
    direct_method = @{ Tag = 'dm'; Slot = 0; LogLevel = 'INFO' }
    transport     = @{ Tag = 'tr'; Slot = 4; SpreadGroups = $true; LogLevel = 'INFO'; Proxy = $true }
    custody       = @{
        Tag = 'cu'; Slot = 10; LogLevel = 'DEBUG'; Pkcs11 = $true
        # OpenSSL handshake errors; a custody failure is otherwise only "TCP/TLS connect failure".
        Environment = @{ AZ_IOT_PAHO_TRACE = 'error' }
    }
}
$adapters = @{ paho = @{ Tag = 'pa'; Index = 0; Suffix = '' }; az_mqtt = @{ Tag = 'az'; Index = 1; Suffix = '_az_mqtt' } }

$suiteName = $env:E2E_SUITE
if (-not $suites.ContainsKey($suiteName)) { throw "Unknown suite '$suiteName'." }
$suite = $suites[$suiteName]
if ($env:E2E_ADAPTER -ne 'both' -and -not $adapters.ContainsKey($env:E2E_ADAPTER)) { throw "Unknown adapter '$($env:E2E_ADAPTER)'." }
$adapter = if ($env:E2E_ADAPTER -eq 'both') { $null } else { $adapters[$env:E2E_ADAPTER] }
if ($env:E2E_SHARD -notmatch '^(\d+)/(\d+)$' -or [int]$Matches[1] -ge [int]$Matches[2]) { throw "Bad shard '$($env:E2E_SHARD)'." }
$shardIndex, $shardCount = [int]$Matches[1], [int]$Matches[2]
if ($suite.ContainsKey('Pkcs11') -and $IsWindows) { throw "Suite $suiteName runs on Linux only." }
# One adapter's test, or every test of the suite.
$targets = @(if ($adapter) { "az_iot_tests_e2e_mqttv5_$suiteName$($adapter.Suffix)" } else { $adapters.Values | ForEach-Object { "az_iot_tests_e2e_mqttv5_$suiteName$($_.Suffix)" } | Sort-Object })
$testPattern = if ($adapter) { "^$($targets[0])$" } else { "e2e_mqttv5_$suiteName" }
$preset, $osTag = if ($IsWindows) { 'windows-msvc-debug', 'w' } else { 'linux-gcc-debug', 'l' }
$legTag = "$osTag$(if ($adapter) { $adapter.Tag })$(if ($shardCount -gt 1) { $shardIndex })"

function Get-SharedValue([string]$Name, [switch]$Secret) {
    $vaultSecret = "E2E-MQTTV5-SHARED-$Name"
    $value = az keyvault secret show --vault-name $env:E2E_SHARED_KEY_VAULT --name $vaultSecret --query value -o tsv --only-show-errors
    if ($LASTEXITCODE -ne 0 -or -not $value) { throw "Could not read $vaultSecret from $($env:E2E_SHARED_KEY_VAULT)." }
    if ($Secret) {
        Write-Host "##vso[task.setsecret]$value"
        # The key alone too, should a test print it outside the connection string.
        if ($value -match 'SharedAccessKey=([^;]+)') { Write-Host "##vso[task.setsecret]$($Matches[1])" }
    }
    return $value
}

function Invoke-Native([string]$What, [scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed ($LASTEXITCODE)." }
}

# An HTTP CONNECT proxy on 127.0.0.1:3128 allowing ports 443 and 8883, logging each CONNECT line:
# tinyproxy on Linux, c/eng/e2e-connect-proxy.py on Windows. Sets AZ_IOT_PROXY_* and
# AZ_IOT_E2E_PROXY_LOG; returns what Stop-ConnectProxy needs.
function Start-ConnectProxy {
    $log = Join-Path $env:AGENT_TEMPDIRECTORY 'connect-proxy.log'
    New-Item -ItemType File -Path $log -Force | Out-Null
    if ($IsWindows) {
        $process = Start-Process python -PassThru -WindowStyle Hidden -ArgumentList @(
            (Join-Path $repositoryRoot 'c/eng/e2e-connect-proxy.py'), '--port', '3128', '--log', $log)
    }
    else {
        # In the foreground (-d) as the agent user, so it can be stopped and its log is readable.
        sudo systemctl stop tinyproxy 2>&1 | Out-Null
        $configuration = Join-Path $env:AGENT_TEMPDIRECTORY 'tinyproxy.conf'
        Set-Content -Path $configuration -Value @(
            'Port 3128', 'Listen 127.0.0.1', 'Timeout 600', "LogFile `"$log`"", 'LogLevel Info',
            'MaxClients 50', 'Allow 127.0.0.1', 'ConnectPort 443', 'ConnectPort 8883')
        $process = Start-Process tinyproxy -PassThru -ArgumentList @('-d', '-c', $configuration)
    }
    $proxy = @{ Process = $process; Log = $log }
    $up = $false
    for ($i = 0; $i -lt 20 -and -not $up; $i++) {
        try { (New-Object System.Net.Sockets.TcpClient('127.0.0.1', 3128)).Dispose(); $up = $true }
        catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $up) { Stop-ConnectProxy $proxy; throw 'The proxy did not start.' }
    $env:AZ_IOT_PROXY_HOST = '127.0.0.1'
    $env:AZ_IOT_PROXY_PORT = '3128'
    $env:AZ_IOT_E2E_PROXY_LOG = $log
    return $proxy
}

function Stop-ConnectProxy($Proxy) {
    Write-Host '--- proxy log'
    Get-Content $Proxy.Log -ErrorAction SilentlyContinue | Write-Host
    Stop-Process -Id $Proxy.Process.Id -Force -ErrorAction SilentlyContinue
}

# Moves the device key into a SoftHSM2 token and deletes the file: the suite reaches it only
# through AZ_IOT_CLIENT_KEY_URI, with the OpenSSL pkcs11 provider active from OPENSSL_CONF alone.
function Move-DeviceKeyToToken([string]$Key) {
    # eval the provider exports so setup-softhsm.sh names pkcs11.so in the OpenSSL configuration.
    $exports = & bash -c 'set -e; mods="$(c/eng/setup-pkcs11-provider.sh)"; eval "$mods"; echo "$mods"; c/eng/setup-softhsm.sh "$AZ_IOT_CLIENT_KEY"'
    if ($LASTEXITCODE -ne 0) { throw "PKCS#11 token provisioning failed ($LASTEXITCODE)." }
    foreach ($line in $exports) {
        if ($line -match "^export ([A-Za-z0-9_]+)='(.*)'$") { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
    Remove-Item Env:AZ_IOT_CLIENT_KEY
    if (Test-Path $Key) { throw 'The device key file is still on disk.' }
    if (-not $env:AZ_IOT_CLIENT_KEY_URI) { throw 'setup-softhsm.sh exported no AZ_IOT_CLIENT_KEY_URI.' }
    Write-Host "--- OPENSSL_CONF = $env:OPENSSL_CONF"
    Get-Content $env:OPENSSL_CONF | Write-Host
    $providers = & openssl list -providers 2>&1 | Out-String
    Write-Host "--- providers active from configuration:`n$providers"
    if ($LASTEXITCODE -ne 0) { throw "openssl list -providers failed ($LASTEXITCODE)." }
    if ($providers -notmatch '(?m)^\s+pkcs11\s*$') {
        throw 'The pkcs11 provider is not active from OPENSSL_CONF; the custody path would fall back to a run-time load.'
    }
}

Set-Location $repositoryRoot
$config = if ($Step -ne 'Build') { Join-Path $env:E2E_DEVICE_DIRECTORY 'test-config.ps1' }
switch ($Step) {
    'Build' {
        if ($IsLinux) {
            # softhsm2 + opensc hold the custody key in a token; meson and pkg-config build the
            # OpenSSL pkcs11 provider (c/eng/setup-pkcs11-provider.sh).
            $packages = @('ninja-build')
            if ($suite.ContainsKey('Proxy')) { $packages += 'tinyproxy' }
            if ($suite.ContainsKey('Pkcs11')) { $packages += 'softhsm2', 'opensc', 'meson', 'pkg-config' }
            Invoke-Native 'Installing packages' { bash c/eng/apt-install.sh @packages }
        }
        $pkcs11 = @(if ($suite.ContainsKey('Pkcs11')) { '-DAZ_IOT_BUILD_E2E_PKCS11=ON' })
        Push-Location c
        try {
            Invoke-Native 'CMake configure' {
                cmake --preset $preset -DAZ_IOT_BUILD_E2E=ON -DAZ_IOT_BUILD_E2E_MQTTV5=ON -DAZ_IOT_WITH_PAHO=ON -DAZ_IOT_WITH_AZ_MQTT=ON @pkcs11
            }
            Invoke-Native 'Build' { cmake --build --preset $preset --config Debug --target @targets }
        }
        finally { Pop-Location }
    }
    'IssueDevice' {
        # GROUP_CA signs devices that the shared enrollment group admits. The hub's 'service'
        # policy, not iothubowner (secret IOTHUB-CS).
        $env:E2E_MQTTV5_SHARED_GROUP_CA = Get-SharedValue GROUP-CA -Secret
        $env:E2E_MQTTV5_SHARED_IOTHUB_CS = Get-SharedValue IOTHUB-SERVICE-CS -Secret
        $env:E2E_MQTTV5_SHARED_EVENTHUB_CS = Get-SharedValue EVENTHUB-CS -Secret
        $env:E2E_MQTTV5_SHARED_ID_SCOPE = Get-SharedValue ID-SCOPE
        $env:E2E_MQTTV5_SHARED_DPS_HOST = Get-SharedValue DPS-HOST
        $directory = $env:E2E_DEVICE_DIRECTORY
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        if (-not $IsWindows) { chmod 700 $directory }
        $slot = $suite.Slot
        if ($suite.ContainsKey('SpreadGroups')) { $slot += $(if ($adapter) { $adapter.Index } else { 0 }) * $shardCount + $shardIndex }
        $group = 'e2e-' + (([long]$env:E2E_BUILD_ID + $slot) % 11)
        $registrationId = "ci-ado-mqttv5-$($suite.Tag)-$($env:E2E_BUILD_ID)-$($env:E2E_JOB_ATTEMPT)-$legTag"
        Write-Host "Device $registrationId, consumer group $group."
        & (Join-Path $repositoryRoot 'c/eng/e2e-shared-device.ps1') -Prefix E2E_MQTTV5_SHARED `
            -RegistrationId $registrationId -ConsumerGroup $group -OutFile $config
        . $config
        $key = Join-Path $directory 'device-key.pem'
        [IO.File]::WriteAllBytes((Join-Path $directory 'device-cert.pem'), [Convert]::FromBase64String($env:IOT_DPS_INDIVIDUAL_X509_CERTIFICATE_0))
        [IO.File]::WriteAllBytes($key, [Convert]::FromBase64String($env:IOT_DPS_INDIVIDUAL_X509_KEY_0))
        if (-not $IsWindows) { chmod 600 $key $config }
    }
    'RunTests' {
        if (-not (Test-Path $config)) { throw 'No device; run IssueDevice first.' }
        . $config
        $directory = $env:E2E_DEVICE_DIRECTORY
        $key = Join-Path $directory 'device-key.pem'
        $env:AZ_IOT_DPS_ID_SCOPE = $env:IOT_DPS_ID_SCOPE
        $env:AZ_IOT_DPS_REGISTRATION_ID = $env:IOT_DPS_INDIVIDUAL_REGISTRATION_ID_0
        $env:AZ_IOT_CLIENT_CERT = Join-Path $directory 'device-cert.pem'
        $env:AZ_IOT_CLIENT_KEY = $key
        $env:AZ_IOT_TRUSTED_CA = & (Join-Path $repositoryRoot 'c/eng/e2e-trusted-ca.ps1') -Path (Join-Path $directory 'trusted-ca.pem')
        if ($env:IOT_DPS_GLOBAL_ENDPOINT) { $env:AZ_IOT_DPS_GLOBAL_ENDPOINT = $env:IOT_DPS_GLOBAL_ENDPOINT }
        if ($shardCount -gt 1) { $env:AZ_IOT_TEST_SHARD = $env:E2E_SHARD }
        $env:AZ_IOT_E2E_LOG_LEVEL = $suite.LogLevel
        if ($suite.ContainsKey('Environment')) { foreach ($name in $suite.Environment.Keys) { Set-Item "env:$name" $suite.Environment[$name] } }
        if ($suite.ContainsKey('Pkcs11')) { Move-DeviceKeyToToken $key }
        $proxy = if ($suite.ContainsKey('Proxy')) { Start-ConnectProxy } else { $null }
        New-Item -ItemType Directory -Force -Path $env:E2E_RESULTS_DIRECTORY | Out-Null
        try {
            ctest --test-dir "c/build/$($preset)" -C Debug -R $testPattern --verbose --no-tests=error --timeout 900 `
                --output-junit (Join-Path $env:E2E_RESULTS_DIRECTORY "mqttv5-$suiteName-$legTag.xml")
            $exitCode = $LASTEXITCODE
        }
        finally { if ($proxy) { Stop-ConnectProxy $proxy } }
        if ($exitCode -ne 0) { throw "mqttv5 $suiteName e2e failed ($exitCode)." }
    }
}
