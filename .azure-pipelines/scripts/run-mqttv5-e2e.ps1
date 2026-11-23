# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$key = [Convert]::FromBase64String($env:CONFIG_KEY)
$encrypted = Get-Content (Join-Path $env:E2E_CONFIG_DIRECTORY 'test-config.encrypted') -Raw
$secure = ConvertTo-SecureString $encrypted.Trim() -Key $key
$config = [Net.NetworkCredential]::new('', $secure).Password | ConvertFrom-Json -AsHashtable
$required = @('IOTHUB_CONNECTION_STRING_MQTTV5', 'IOT_DPS_CONNECTION_STRING_MQTTV5',
    'IOT_DPS_ID_SCOPE_MQTTV5', 'IOT_DPS_GLOBAL_ENDPOINT_MQTTV5')
$previous = @{}
try {
    foreach ($name in $required) {
        if ([string]::IsNullOrWhiteSpace($config[$name])) { throw "Missing $name in test configuration." }
        $previous[$name] = [Environment]::GetEnvironmentVariable($name)
        $value = $config[$name].Replace('%', '%AZP25').Replace("`r", '%0D').Replace("`n", '%0A')
        Write-Host "##vso[task.setsecret]$value"
        [Environment]::SetEnvironmentVariable($name, $config[$name])
    }
    $results = Join-Path $env:COMMON_TESTRESULTSDIRECTORY 'mqttv5'
    $null = New-Item -ItemType Directory -Force -Path $results
    # Published with the raw test results, to diagnose failures from the client's side.
    $previous['E2E_CLIENT_TRACE_FILE'] = $env:E2E_CLIENT_TRACE_FILE
    $env:E2E_CLIENT_TRACE_FILE = Join-Path $results 'client-trace.log'
    $project = Join-Path $PSScriptRoot '../../dotnet/tests/Microsoft.Azure.Iot.Device.IntegrationTests/Microsoft.Azure.Iot.Device.IntegrationTests.csproj'
    & dotnet test $project --no-build `
        --filter 'Hub=MQTTv5&Category!=CertificateManagement&Category!=Longhaul' `
        --logger 'trx;LogFileName=test-results.trx' --results-directory $results `
        -- xunit.parallelizeAssembly=true
    if ($LASTEXITCODE -ne 0) { throw "MQTTv5 integration tests failed (exit $LASTEXITCODE)." }
    $reportPath = Join-Path $results 'test-results.trx'
    if (-not (Test-Path -LiteralPath $reportPath)) { throw 'MQTTv5 tests produced no TRX report.' }
    [xml]$report = Get-Content -LiteralPath $reportPath -Raw
    if ([int]$report.TestRun.ResultSummary.Counters.executed -lt 1) {
        throw 'No MQTTv5 tests executed; check the filter and test prerequisites.'
    }
}
finally {
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name])
    }
    $config = $null
    $key = $null
}
