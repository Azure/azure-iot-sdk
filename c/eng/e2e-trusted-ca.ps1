# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
Writes the host's trusted root CAs to a PEM bundle for the e2e tests (AZ_IOT_TRUSTED_CA).

.DESCRIPTION
Linux: the OpenSSL system bundle. Windows: the LocalMachine and CurrentUser Root stores.
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
    $pem = New-Object System.Text.StringBuilder
    foreach ($location in @('LocalMachine', 'CurrentUser')) {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', $location)
        $store.Open('ReadOnly')
        foreach ($certificate in $store.Certificates) { [void]$pem.AppendLine($certificate.ExportCertificatePem()) }
        $store.Close()
    }
    [System.IO.File]::WriteAllText($Path, $pem.ToString())
}
$Path
