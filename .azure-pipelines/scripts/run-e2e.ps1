# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

$ErrorActionPreference = 'Stop'
$key = [Convert]::FromBase64String($env:CONFIG_KEY)
$encrypted = Get-Content (Join-Path $env:E2E_CONFIG_DIRECTORY 'test-config.encrypted') -Raw
$secureConfig = ConvertTo-SecureString $encrypted.Trim() -Key $key
$config = [Net.NetworkCredential]::new('', $secureConfig).Password
$configPath = Join-Path $env:AGENT_TEMPDIRECTORY ("test-config-" + [Guid]::NewGuid().ToString('N') + '.ps1')
$certificateDirectory = $null
$exitCode = 0
try {
    Set-Content $configPath $config
    . $configPath
    foreach ($item in (Get-ChildItem Env: | Where-Object { $_.Name -match '^(IOTHUB_|IOT_DPS_|PROVISIONING_)' })) {
        $value = $item.Value.Replace('%', '%AZP25').Replace("`r", '%0D').Replace("`n", '%0A')
        Write-Host "##vso[task.setsecret]$value"
    }

    $env:AZ_IOT_DPS_ID_SCOPE = $env:IOT_DPS_ID_SCOPE
    if ($env:IOT_DPS_GLOBAL_ENDPOINT) { $env:AZ_IOT_DPS_GLOBAL_ENDPOINT = $env:IOT_DPS_GLOBAL_ENDPOINT }
    $testArguments = @('--no-build', '--logger', 'trx')
    # Each project gets its own results directory so the pipeline can publish them as separately named runs.
    $projects = [ordered]@{
        integration = './dotnet/tests/Microsoft.Azure.Iot.Device.IntegrationTests/Microsoft.Azure.Iot.Device.IntegrationTests.csproj'
    }

    switch ($env:E2E_SUITE) {
        'standard' {
            $projects['unit'] = './dotnet/tests/Microsoft.Azure.Iot.Device.UnitTests/Microsoft.Azure.Iot.Device.UnitTests.csproj'
            $testArguments += @('--filter', 'Category!=CertificateManagement&Category!=Longhaul&Category!=FileUpload&Hub!=MQTTv5')
            $testArguments += @('--collect', 'XPlat Code Coverage', '--', 'xunit.parallelizeAssembly=true')
        }
        'csr' {
            $index = $env:DEVICE_INDEX
            $registration = [Environment]::GetEnvironmentVariable("IOT_DPS_INDIVIDUAL_REGISTRATION_ID_$index")
            $certificate = [Environment]::GetEnvironmentVariable("IOT_DPS_INDIVIDUAL_X509_CERTIFICATE_$index")
            $privateKey = [Environment]::GetEnvironmentVariable("IOT_DPS_INDIVIDUAL_X509_KEY_$index")
            if (-not $registration -or -not $certificate -or -not $privateKey) {
                $registration = $env:IOT_DPS_INDIVIDUAL_REGISTRATION_ID
                $certificate = $env:IOT_DPS_INDIVIDUAL_X509_CERTIFICATE
                $privateKey = $env:IOT_DPS_INDIVIDUAL_X509_KEY
            }
            foreach ($name in @('IOT_DPS_GROUP_X509_REGISTRATION_ID', 'IOT_DPS_GROUP_X509_CERTIFICATE',
                    'IOT_DPS_GROUP_X509_KEY', 'IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY')) {
                if (-not [Environment]::GetEnvironmentVariable($name)) { throw "Missing $name in test configuration." }
            }
            $certificateDirectory = Join-Path $env:AGENT_TEMPDIRECTORY ([Guid]::NewGuid().ToString('N'))
            New-Item -ItemType Directory $certificateDirectory | Out-Null
            $env:AZ_IOT_DPS_REGISTRATION_ID = $registration
            $env:AZ_IOT_CLIENT_CERT = Join-Path $certificateDirectory 'device-cert.pem'
            $env:AZ_IOT_CLIENT_KEY = Join-Path $certificateDirectory 'device-key.pem'
            $env:AZ_IOT_TRUSTED_CA = Join-Path $certificateDirectory 'trusted-ca.pem'
            [IO.File]::WriteAllBytes($env:AZ_IOT_CLIENT_CERT, [Convert]::FromBase64String($certificate))
            [IO.File]::WriteAllBytes($env:AZ_IOT_CLIENT_KEY, [Convert]::FromBase64String($privateKey))
            if ($IsLinux) {
                Copy-Item '/etc/ssl/certs/ca-certificates.crt' $env:AZ_IOT_TRUSTED_CA
            }
            else {
                $bundle = [Text.StringBuilder]::new()
                foreach ($location in @('LocalMachine', 'CurrentUser')) {
                    $store = [Security.Cryptography.X509Certificates.X509Store]::new('Root', $location)
                    try {
                        $store.Open('ReadOnly')
                        foreach ($cert in $store.Certificates) { [void]$bundle.AppendLine($cert.ExportCertificatePem()) }
                    }
                    finally { $store.Close() }
                }
                [IO.File]::WriteAllText($env:AZ_IOT_TRUSTED_CA, $bundle.ToString())
            }
            $testArguments += @('--filter', 'Category=CertificateManagement&Category!=FileUpload&Hub!=MQTTv5')
            $testArguments += @('--collect', 'XPlat Code Coverage', '--', 'xunit.parallelizeAssembly=true')
        }
        'longhaul' {
            foreach ($name in @('IOTHUB_CONNECTION_STRING', 'IOT_DPS_CONNECTION_STRING', 'IOT_DPS_ID_SCOPE')) {
                if (-not [Environment]::GetEnvironmentVariable($name)) { throw "Missing $name in test configuration." }
            }
            $env:LONGHAUL_OUTPUT_DIRECTORY = Join-Path $env:BUILD_SOURCESDIRECTORY 'longhaul-results'
            New-Item -ItemType Directory -Force $env:LONGHAUL_OUTPUT_DIRECTORY | Out-Null
            $testArguments += @('--filter', 'Category=Longhaul&LonghaulScenario=UnifiedMQTTv3Hub')
            $testArguments += @('--logger', 'console;verbosity=normal', '--', 'xunit.diagnosticMessages=true')
        }
        default { throw 'Unknown test suite.' }
    }

    foreach ($entry in $projects.GetEnumerator()) {
        $resultsDirectory = Join-Path $env:COMMON_TESTRESULTSDIRECTORY $entry.Key
        & dotnet test $entry.Value --results-directory $resultsDirectory @testArguments
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
    }
    if ($exitCode -eq 0 -and $env:E2E_SUITE -eq 'longhaul' -and
        -not (Test-Path (Join-Path $env:LONGHAUL_OUTPUT_DIRECTORY 'UnifiedMQTTv3Hub.json'))) {
        throw 'Longhaul produced no report; the test may have been skipped or the filter matched no tests.'
    }
}
finally {
    Remove-Item $configPath -Force -ErrorAction SilentlyContinue
    if ($certificateDirectory) {
        Remove-Item (Join-Path $certificateDirectory 'device-cert.pem') -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $certificateDirectory 'device-key.pem') -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $certificateDirectory 'trusted-ca.pem') -Force -ErrorAction SilentlyContinue
        Remove-Item $certificateDirectory -Force -ErrorAction SilentlyContinue
    }
}
exit $exitCode
