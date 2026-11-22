# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Runs one C samples job (jobs-c-e2e.yml): the samples of its adapter (and shard), serially, on the
# device of its slot. Each sample is a CTest test (c/tests/e2e/tests/e2e_samples_test.c) that starts
# the sample, waits for its end and checks what the service saw, with Microsoft Entra tokens:
# IOTHUB_SERVICE_ACCESS_TOKEN for hub service calls, IOTHUB_EVENTHUB_ACCESS_TOKEN to read the Event
# Hub that telemetry is routed to, on consumer group samples-<slot>. With E2E_SAMPLES_CSR, telemetry
# is read from the hub's built-in endpoint with its shared access key instead (provision.ps1).
#
# The device is enrollment E2E_SAMPLES_SLOT: X.509 individual (IOT_DPS_INDIVIDUAL_*_<slot>) or, with
# E2E_SAMPLES_CSR, a bootstrap identity under the certificate-management X.509 group
# (IOT_DPS_GROUP_X509_*_<slot>). Only the service variables the test reads are left in the
# environment, so a sample sees nothing of the test configuration but its device's.
#
# Copies each sample's output and the proxy's log to E2E_LOGS_DIRECTORY and then sets variable
# samplesLogs.

$ErrorActionPreference = 'Stop'

$csr = $env:E2E_SAMPLES_CSR -eq 'true'
$required = @('IOTHUB_SERVICE_ACCESS_TOKEN', 'E2E_CONFIG_DIRECTORY', 'E2E_PRESET', 'E2E_SAMPLES_ADAPTER',
              'E2E_SAMPLES_SLOT', 'E2E_RESULTS_DIRECTORY', 'E2E_LOGS_DIRECTORY')
if ($csr) {
    if ($env:IOTHUB_EVENTHUB_ACCESS_TOKEN) { throw 'IOTHUB_EVENTHUB_ACCESS_TOKEN is set: the CSR samples read the built-in endpoint.' }
} else {
    $required += @('IOTHUB_EVENTHUB_ACCESS_TOKEN', 'E2E_SAMPLES_SHARD', 'E2E_PROXY_VENV')
}
foreach ($name in $required) {
    if (-not (Get-Item "env:$name" -ErrorAction SilentlyContinue).Value) { throw "$name is not set." }
}
$adapter = $env:E2E_SAMPLES_ADAPTER
$slot = [int]$env:E2E_SAMPLES_SLOT

$before = @(Get-ChildItem env: | ForEach-Object Name)
& (Join-Path $PSScriptRoot 'import-test-config.ps1')
$config = @{}
foreach ($name in @(Get-ChildItem env: | ForEach-Object Name | Where-Object { $_ -notin $before })) {
    $config[$name] = (Get-Item "env:$name").Value
    Remove-Item "env:$name"
}
foreach ($name in 'IOTHUB_CONNECTION_STRING', 'IOTHUB_HOSTNAME', 'IOTHUB_EVENTHUB_CONNECTION_STRING',
                  'IOTHUB_EVENTHUB_LISTEN_NAME', 'IOTHUB_EVENTHUB_PARTITION_COUNT', 'IOT_DPS_CONNECTION_STRING',
                  'IOTHUB_REGISTRY_CONNECTION_STRING') {
    if ($config[$name]) { Set-Item "env:$name" $config[$name] }
}
$env:IOTHUB_EVENTHUB_CONSUMER_GROUP = "samples-$slot"

$deviceVariables = if ($csr) {
    @('IOT_DPS_GROUP_X509_REGISTRATION_ID', 'IOT_DPS_GROUP_X509_CERTIFICATE', 'IOT_DPS_GROUP_X509_KEY')
} else {
    @('IOT_DPS_INDIVIDUAL_REGISTRATION_ID', 'IOT_DPS_INDIVIDUAL_X509_CERTIFICATE', 'IOT_DPS_INDIVIDUAL_X509_KEY')
}
$registrationId, $certificateBase64, $keyBase64 = foreach ($name in $deviceVariables) { $config["$($name)_$slot"] }
if (-not $registrationId -or -not $certificateBase64 -or -not $keyBase64) {
    throw "The test configuration has no X.509 device $slot ($($deviceVariables[0])_$slot)."
}
if (-not $csr -and -not $config['E2E_EVENTHUB_READABLE_AFTER']) {
    throw 'The test configuration has no E2E_EVENTHUB_READABLE_AFTER.'
}
if (-not $config['IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY']) {
    throw 'The test configuration has no DPS symmetric-key enrollment group (IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY).'
}
Write-Host "samples device (slot $slot): $registrationId"

$deviceDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("az-iot-c-samples-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $deviceDirectory -Force | Out-Null
$buildDirectory = "c/build/$($env:E2E_PRESET)"
$proxy = $null
$proxyFiles = @()
$failed = $true
try {
    $env:AZ_IOT_CLIENT_CERT = Join-Path $deviceDirectory 'device-cert.pem'
    $env:AZ_IOT_CLIENT_KEY = Join-Path $deviceDirectory 'device-key.pem'
    [System.IO.File]::WriteAllBytes($env:AZ_IOT_CLIENT_CERT, [Convert]::FromBase64String($certificateBase64))
    [System.IO.File]::WriteAllBytes($env:AZ_IOT_CLIENT_KEY, [Convert]::FromBase64String($keyBase64))
    $env:AZ_IOT_DPS_ID_SCOPE = $config['IOT_DPS_ID_SCOPE']
    $env:AZ_IOT_DPS_REGISTRATION_ID = $registrationId
    $env:AZ_IOT_TRUSTED_CA = & ./c/eng/e2e-trusted-ca.ps1 -Path (Join-Path $deviceDirectory 'trusted-ca.pem')
    if ($config['IOT_DPS_GLOBAL_ENDPOINT']) { $env:AZ_IOT_DPS_GLOBAL_ENDPOINT = $config['IOT_DPS_GLOBAL_ENDPOINT'] }
    $env:AZ_IOT_DPS_ENROLLMENT_GROUP_KEY = $config['IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY']

    if (-not $csr) {
        if ($IsLinux) {
            # hsm_pkcs11: a copy of the device key in a SoftHSM2 token. Its OpenSSL configuration
            # (the pkcs11 provider) is for that sample only: the test passes it as OPENSSL_CONF.
            $exports = & bash -c 'set -e; mods="$(c/eng/setup-pkcs11-provider.sh)"; eval "$mods"; c/eng/setup-softhsm.sh "$AZ_IOT_CLIENT_KEY" --keep-key-file'
            if ($LASTEXITCODE -ne 0) { throw "PKCS#11 token provisioning failed ($LASTEXITCODE)." }
            foreach ($line in $exports) {
                if ($line -match "^export ([A-Za-z0-9_]+)='(.*)'$") {
                    $name = $Matches[1] -eq 'OPENSSL_CONF' ? 'AZ_IOT_SAMPLES_E2E_PKCS11_OPENSSL_CONF' : $Matches[1]
                    Set-Item "env:$name" $Matches[2]
                }
            }
        }

        # The HTTP proxy of unified/proxy, for this script's lifetime.
        $env:AZ_IOT_PROXY_HOST = '127.0.0.1'
        $env:AZ_IOT_PROXY_PORT = '8899'
        $env:AZ_IOT_SAMPLES_E2E_PROXY_LOG = Join-Path $deviceDirectory 'proxy.log'
        $proxyFiles = @($env:AZ_IOT_SAMPLES_E2E_PROXY_LOG, (Join-Path $deviceDirectory 'proxy.out'),
                        (Join-Path $deviceDirectory 'proxy.err'))
        $python = Join-Path $env:E2E_PROXY_VENV ($IsWindows ? 'Scripts/python.exe' : 'bin/python')
        $proxy = Start-Process -FilePath $python -PassThru -ArgumentList @('-m', 'proxy', '--hostname', '127.0.0.1',
            '--port', $env:AZ_IOT_PROXY_PORT, '--log-level', 'INFO', '--log-file', $env:AZ_IOT_SAMPLES_E2E_PROXY_LOG) `
            -RedirectStandardOutput $proxyFiles[1] -RedirectStandardError $proxyFiles[2]
        $listening = $false
        for ($i = 0; $i -lt 60 -and -not $listening; $i++) {
            $client = [System.Net.Sockets.TcpClient]::new()
            try { $client.Connect('127.0.0.1', [int]$env:AZ_IOT_PROXY_PORT); $listening = $true }
            catch { Start-Sleep -Milliseconds 500 }
            finally { $client.Dispose() }
        }
        if (-not $listening) { throw "proxy.py did not listen on $($env:AZ_IOT_PROXY_PORT)." }
    }

    # Until the Event Hubs Data Receiver role applies, reading the routed telemetry is refused
    # (provision.ps1).
    $wait = if ($csr) { 0 } else {
        [long]$config['E2E_EVENTHUB_READABLE_AFTER'] - [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    }
    if ($wait -gt 0) {
        Write-Host "Waiting $wait seconds for the Event Hubs role assignments to apply."
        Start-Sleep -Seconds $wait
    }

    # Shard 0; shard 1 is every other test. Unified samples run ~70 s each.
    $shard0 = '^az_iot_samples_e2e_(unified_(telemetry|connect_first|websockets|proxy|multithreaded_telemetry)|authentication_(dps_sas_key|user_provided_sas_token))_'
    if ($csr) {
        $label = 'samples_csr'
        $select = @('-R', "_$adapter$")
        $leg = "csr-$($env:AGENT_OS)-$adapter"
    } else {
        $label = 'samples_mqttv3'
        $select = if ($env:E2E_SAMPLES_SHARD -eq '0') { @('-R', "$shard0$adapter$") } else { @('-R', "_$adapter$", '-E', $shard0) }
        $leg = "$($env:AGENT_OS)-$adapter-$($env:E2E_SAMPLES_SHARD)"
    }
    New-Item -ItemType Directory -Path $env:E2E_RESULTS_DIRECTORY -Force | Out-Null
    ctest --test-dir $buildDirectory -C Debug -L $label @select --verbose --no-tests=error `
        --output-junit (Join-Path $env:E2E_RESULTS_DIRECTORY "c-samples-$leg.xml")
    $failed = $LASTEXITCODE -ne 0
    if ($failed) { Write-Host "##vso[task.logissue type=error]$leg samples failed ($LASTEXITCODE)." }
}
finally {
    if ($proxy) { Stop-Process -Id $proxy.Id -ErrorAction SilentlyContinue; $proxy.WaitForExit(10000) | Out-Null }
    $samplesDirectory = Join-Path $buildDirectory 'tests/e2e/samples'
    $logs = @(Get-ChildItem -Path $samplesDirectory -Filter 'sample-output.log' -Recurse -File -ErrorAction SilentlyContinue)
    $logs += @($proxyFiles | Where-Object { Test-Path $_ } | Get-Item)
    if ($logs.Count -gt 0) {
        New-Item -ItemType Directory -Path $env:E2E_LOGS_DIRECTORY -Force | Out-Null
        $samplesRoot = (Resolve-Path $samplesDirectory -ErrorAction SilentlyContinue).Path
        foreach ($log in $logs) {
            # Each sample's output under its test's directory name.
            $relative = if ($samplesRoot -and $log.FullName.StartsWith($samplesRoot)) {
                [System.IO.Path]::GetRelativePath($samplesRoot, $log.FullName)
            } else { $log.Name }
            $target = Join-Path $env:E2E_LOGS_DIRECTORY $relative
            New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
            Copy-Item $log.FullName $target -Force
        }
        Write-Host '##vso[task.setvariable variable=samplesLogs]true'
    }
    Remove-Item -Recurse -Force $deviceDirectory -ErrorAction SilentlyContinue
}
if ($failed) { exit 1 }
