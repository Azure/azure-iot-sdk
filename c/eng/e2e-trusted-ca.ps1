# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
Writes the host's trusted root CAs to a PEM bundle for the e2e tests (AZ_IOT_TRUSTED_CA).

.DESCRIPTION
Linux: the OpenSSL system bundle. Windows: the unexpired roots of the LocalMachine and CurrentUser
Root stores, each once.
Nothing is downloaded. Prints the bundle's path.

.EXAMPLE
$env:AZ_IOT_TRUSTED_CA = ./c/eng/e2e-trusted-ca.ps1 -Path (Join-Path $dir 'trusted-ca.pem')
#>
param(
    [Parameter(Mandatory)]
    [string] $Path
)

$ErrorActionPreference = 'Stop'

$systemBundle = '/etc/ssl/certs/ca-certificates.crt'
if ($IsLinux -and (Test-Path $systemBundle)) {
    Copy-Item $systemBundle $Path
}
else {
    # Each root once (CurrentUser's Root store lists LocalMachine's too) and only unexpired ones,
    # which are all a chain can end at: libcurl over Schannel refuses a CA file over 1 MiB.
    $pem = New-Object System.Text.StringBuilder
    $seen = @{}
    $now = Get-Date
    foreach ($location in @('LocalMachine', 'CurrentUser')) {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', $location)
        $store.Open('ReadOnly')
        foreach ($certificate in $store.Certificates) {
            if ($certificate.NotAfter -lt $now -or $seen.ContainsKey($certificate.Thumbprint)) { continue }
            $seen[$certificate.Thumbprint] = $true
            [void]$pem.AppendLine($certificate.ExportCertificatePem())
        }
        $store.Close()
    }
    [System.IO.File]::WriteAllText($Path, $pem.ToString())
    Write-Host "trusted CA bundle: $($seen.Count) roots, $((Get-Item $Path).Length) bytes"
}
$Path
