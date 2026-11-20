# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Decrypts the test configuration published by provision.ps1 into this process's environment and
# masks its IOTHUB_*, IOT_DPS_* and PROVISIONING_* values in the log. Needs CONFIG_KEY and
# E2E_CONFIG_DIRECTORY.
$ErrorActionPreference = 'Stop'
$key = [Convert]::FromBase64String($env:CONFIG_KEY)
$encrypted = Get-Content (Join-Path $env:E2E_CONFIG_DIRECTORY 'test-config.encrypted') -Raw
$secureConfig = ConvertTo-SecureString $encrypted.Trim() -Key $key
$config = [Net.NetworkCredential]::new('', $secureConfig).Password
$configPath = Join-Path $env:AGENT_TEMPDIRECTORY ("test-config-" + [Guid]::NewGuid().ToString('N') + '.ps1')
try {
    Set-Content $configPath $config
    . $configPath
    foreach ($item in (Get-ChildItem Env: | Where-Object { $_.Name -match '^(IOTHUB_|IOT_DPS_|PROVISIONING_)' })) {
        $value = $item.Value.Replace('%', '%AZP25').Replace("`r", '%0D').Replace("`n", '%0A')
        Write-Host "##vso[task.setsecret]$value"
    }
}
finally {
    Remove-Item $configPath -Force -ErrorAction SilentlyContinue
}
