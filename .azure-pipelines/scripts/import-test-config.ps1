# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Loads the test configuration into this process's environment and masks its IOTHUB_*, IOT_DPS_*
# and PROVISIONING_* values of 8 or more characters in the log. With CONFIG_KEY, decrypts the configuration published by
# provision.ps1 (E2E_CONFIG_DIRECTORY/test-config.encrypted); without, reads and deletes the
# plaintext one that get-c-e2e-access.ps1 wrote in this job (E2E_CONFIG_DIRECTORY/set_test_env_vars.ps1).
$ErrorActionPreference = 'Stop'
$configPath = Join-Path $env:AGENT_TEMPDIRECTORY ("test-config-" + [Guid]::NewGuid().ToString('N') + '.ps1')
if ($env:CONFIG_KEY) {
    $key = [Convert]::FromBase64String($env:CONFIG_KEY)
    # Layout written by provision.ps1: nonce (12 bytes), tag (16 bytes), then ciphertext.
    $encrypted = [IO.File]::ReadAllBytes((Join-Path $env:E2E_CONFIG_DIRECTORY 'test-config.encrypted'))
    $nonce = [byte[]]::new(12)
    $tag = [byte[]]::new(16)
    $ciphertext = [byte[]]::new($encrypted.Length - 28)
    [Array]::Copy($encrypted, 0, $nonce, 0, 12)
    [Array]::Copy($encrypted, 12, $tag, 0, 16)
    [Array]::Copy($encrypted, 28, $ciphertext, 0, $ciphertext.Length)
    $plaintext = [byte[]]::new($ciphertext.Length)
    $aes = [Security.Cryptography.AesGcm]::new($key, 16)
    try { $aes.Decrypt($nonce, $ciphertext, $tag, $plaintext) } finally { $aes.Dispose() }
    [IO.File]::WriteAllBytes($configPath, $plaintext)
} else {
    Move-Item (Join-Path $env:E2E_CONFIG_DIRECTORY 'set_test_env_vars.ps1') $configPath
}
try {
    . $configPath
    # Short values (partition count, consumer group) are not secret; masking them would redact
    # every occurrence of, say, '4' in the log.
    foreach ($item in (Get-ChildItem Env: | Where-Object { $_.Name -match '^(IOTHUB_|IOT_DPS_|PROVISIONING_)' -and $_.Value.Length -ge 8 })) {
        $value = $item.Value.Replace('%', '%AZP25').Replace("`r", '%0D').Replace("`n", '%0A')
        Write-Host "##vso[task.setsecret]$value"
    }
}
finally {
    Remove-Item $configPath -Force -ErrorAction SilentlyContinue
}
