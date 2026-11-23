# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
Writes the e2e test config for one device on a shared e2e environment.

.DESCRIPTION
Issues a device certificate for -RegistrationId, signed by the shared DPS X.509 enrollment
group's CA, and writes the test-config script the workflow sources (the variables the
provisioning action emits), including the shared symmetric-key group's key for the SAS
suites. Devices register through the groups on first connect, so no Azure call is made here.

Default, inputs from the environment (repository secrets/variables):
  E2E_SHARED_GROUP_CA        base64 of a PEM holding the group CA certificate and its private key
  E2E_SHARED_IOTHUB_CS       IoT Hub 'service' policy connection string
  E2E_SHARED_EVENTHUB_CS     Event Hub-compatible endpoint connection string, 'service' policy
  E2E_SHARED_ID_SCOPE        DPS ID scope
  E2E_SHARED_SAS_GROUP_KEY   primary key of the DPS symmetric-key enrollment group
  E2E_SHARED_DPS_CS          DPS service connection string (EnrollmentWrite, RegistrationStatusWrite),
                             written as IOT_DPS_CONNECTION_STRING for the TPM suite's enrollments
  E2E_SHARED_IOTHUB_REGISTRY_CS  IoT Hub connection string with RegistryWrite, written as
                             IOTHUB_REGISTRY_CONNECTION_STRING for the TPM suite's device cleanup

-Prefix E2E_MQTTV5_SHARED (ci-c-e2e-mqttv5) reads the same variables under that prefix except
SAS_GROUP_KEY, DPS_CS and IOTHUB_REGISTRY_CS (mqttv5 hubs accept X.509 only; no SAS key is
written, and there is no TPM suite), and optionally
E2E_MQTTV5_SHARED_DPS_HOST, the DPS device endpoint.

-EntraAuthentication (Azure Pipelines ci-c-e2e) is for a hub and Event Hub read with Microsoft
Entra tokens, which the caller sets (IOTHUB_SERVICE_ACCESS_TOKEN, IOTHUB_EVENTHUB_ACCESS_TOKEN). It
reads no IoT Hub or Event Hubs keys: instead of E2E_SHARED_IOTHUB_CS, E2E_SHARED_EVENTHUB_CS and
E2E_SHARED_IOTHUB_REGISTRY_CS:
  E2E_SHARED_IOTHUB_HOSTNAME          IoT Hub host name
  E2E_SHARED_EVENTHUB_KEYLESS_CS      Event Hub connection string without a key (Endpoint, EntityPath)
  E2E_SHARED_EVENTHUB_PARTITION_COUNT that Event Hub's partition count (optional; tests default to 4)

-Csr (Azure Pipelines ci-c-e2e, CSR suites) writes the IOT_DPS_GROUP_X509_*_<DeviceIndex> bootstrap
identity instead, from:
  E2E_CSR_SHARED_GROUP_CA    base64 of a PEM holding the group's issuing CA certificate and its
                             private key, then any CA certificates above it (issuer first)
  E2E_CSR_SHARED_ID_SCOPE    DPS ID scope
  E2E_CSR_SHARED_DPS_HOST    DPS device endpoint (optional)
  E2E_CSR_SHARED_SAS_GROUP_KEY  primary key of the DPS symmetric-key enrollment group, linked to
                             the certificate policy
#>
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,127}$')][string]$RegistrationId,
    [ValidateRange(0, 99)][int]$DeviceIndex = 0,
    [ValidatePattern('^[A-Za-z0-9$._-]{1,50}$')][string]$ConsumerGroup = '$Default',
    # Variable prefix for the default (non -Csr) mode.
    [ValidateSet('E2E_SHARED', 'E2E_MQTTV5_SHARED')][string]$Prefix = 'E2E_SHARED',
    [switch]$Csr,
    [switch]$EntraAuthentication,
    [string]$OutFile = 'test_config/set_test_env_vars.ps1'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Csr -and $EntraAuthentication) { throw '-Csr and -EntraAuthentication cannot be combined.' }
if ($EntraAuthentication -and $Prefix -ne 'E2E_SHARED') { throw '-EntraAuthentication supports only -Prefix E2E_SHARED.' }

$Vars = if ($Csr) { 'E2E_CSR_SHARED' } else { $Prefix }
function Get-Shared([string]$Name) { [Environment]::GetEnvironmentVariable("${Vars}_$Name") }
$Mqttv5 = -not $Csr -and $Prefix -eq 'E2E_MQTTV5_SHARED'
$Required = if ($Csr) { @('GROUP_CA', 'ID_SCOPE', 'SAS_GROUP_KEY') }
            elseif ($Mqttv5) { @('GROUP_CA', 'IOTHUB_CS', 'EVENTHUB_CS', 'ID_SCOPE') }
            elseif ($EntraAuthentication) { @('GROUP_CA', 'IOTHUB_HOSTNAME', 'EVENTHUB_KEYLESS_CS', 'ID_SCOPE', 'SAS_GROUP_KEY', 'DPS_CS') }
            else { @('GROUP_CA', 'IOTHUB_CS', 'EVENTHUB_CS', 'ID_SCOPE', 'SAS_GROUP_KEY', 'DPS_CS', 'IOTHUB_REGISTRY_CS') }
$Missing = $Required | ForEach-Object { "${Vars}_$_" } |
    Where-Object { [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($_)) }
if ($Missing) { throw "Shared e2e environment not configured; missing: $($Missing -join ', ')." }

if ($EntraAuthentication) {
    if ((Get-Shared 'EVENTHUB_KEYLESS_CS') -match 'SharedAccessKey') {
        throw "${Vars}_EVENTHUB_KEYLESS_CS holds a key."
    }
} elseif (-not $Csr) {
    foreach ($Name in "${Vars}_IOTHUB_CS", "${Vars}_EVENTHUB_CS") {
        if ([Environment]::GetEnvironmentVariable($Name) -match 'SharedAccessKeyName=iothubowner') {
            throw "$Name uses the iothubowner policy; use the 'service' policy."
        }
    }
}

$CaPem = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([Environment]::GetEnvironmentVariable("${Vars}_GROUP_CA")))
# The first certificate is the issuer; the key must match it.
$Ca = [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPem($CaPem, $CaPem)
if (-not $Ca.HasPrivateKey) { throw "${Vars}_GROUP_CA has no private key." }
$CaChain = [System.Security.Cryptography.X509Certificates.X509Certificate2Collection]::new()
$CaChain.ImportFromPem($CaPem)

$Rsa = [System.Security.Cryptography.RSA]::Create(2048)
$Request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
    "CN=$RegistrationId", $Rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
    [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor
        [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyEncipherment, $true))
$Eku = [System.Security.Cryptography.OidCollection]::new()
[void]$Eku.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.2'))
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($Eku, $false))
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($Request.PublicKey, $false))
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509AuthorityKeyIdentifierExtension]::CreateFromCertificate($Ca, $true, $false))

$Serial = [byte[]]::new(16)
[System.Security.Cryptography.RandomNumberGenerator]::Fill($Serial)
$Serial[0] = $Serial[0] -band 0x7F
$Now = [DateTimeOffset]::UtcNow
$NotBefore = $Now.AddMinutes(-10)
if ($NotBefore -lt $Ca.NotBefore) { $NotBefore = [DateTimeOffset]$Ca.NotBefore }
$NotAfter = $Now.AddDays(2)
if ($NotAfter -gt $Ca.NotAfter) { $NotAfter = [DateTimeOffset]$Ca.NotAfter }
$Leaf = $Request.Create($Ca, $NotBefore, $NotAfter, $Serial)

function ConvertTo-B64([string]$Text) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text)) }
function Format-Assignment([string]$Name, [string]$Value) { "`$env:$Name = '$($Value.Replace("'", "''"))'" }

# Chain: leaf + the bundle's CA certificates. DPS matches the group by the issuing CA.
$Chain = $Leaf.ExportCertificatePem() + "`n" + (($CaChain | ForEach-Object { $_.ExportCertificatePem() }) -join "`n") + "`n"
$KeyB64 = ConvertTo-B64 $Rsa.ExportRSAPrivateKeyPem()
$Lines = if ($Csr) {
    @(
        Format-Assignment 'IOT_DPS_ID_SCOPE' $env:E2E_CSR_SHARED_ID_SCOPE
        Format-Assignment "IOT_DPS_GROUP_X509_REGISTRATION_ID_$DeviceIndex" $RegistrationId
        Format-Assignment "IOT_DPS_GROUP_X509_CERTIFICATE_$DeviceIndex" (ConvertTo-B64 $Chain)
        Format-Assignment "IOT_DPS_GROUP_X509_KEY_$DeviceIndex" $KeyB64
        if ($env:E2E_CSR_SHARED_DPS_HOST) { Format-Assignment 'IOT_DPS_GLOBAL_ENDPOINT' $env:E2E_CSR_SHARED_DPS_HOST }
        Format-Assignment 'IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY' $env:E2E_CSR_SHARED_SAS_GROUP_KEY
    )
} elseif ($EntraAuthentication) {
    @(
        Format-Assignment 'IOTHUB_HOSTNAME' (Get-Shared 'IOTHUB_HOSTNAME')
        Format-Assignment 'IOTHUB_EVENTHUB_CONNECTION_STRING' (Get-Shared 'EVENTHUB_KEYLESS_CS')
        if (Get-Shared 'EVENTHUB_PARTITION_COUNT') {
            Format-Assignment 'IOTHUB_EVENTHUB_PARTITION_COUNT' (Get-Shared 'EVENTHUB_PARTITION_COUNT')
        }
        # The shared environment's role assignments already apply.
        Format-Assignment 'E2E_EVENTHUB_READABLE_AFTER' '0'
        Format-Assignment 'IOTHUB_EVENTHUB_CONSUMER_GROUP' $ConsumerGroup
        Format-Assignment 'IOT_DPS_ID_SCOPE' (Get-Shared 'ID_SCOPE')
        Format-Assignment "IOT_DPS_INDIVIDUAL_REGISTRATION_ID_$DeviceIndex" $RegistrationId
        Format-Assignment "IOT_DPS_INDIVIDUAL_X509_CERTIFICATE_$DeviceIndex" (ConvertTo-B64 $Chain)
        Format-Assignment "IOT_DPS_INDIVIDUAL_X509_KEY_$DeviceIndex" $KeyB64
        Format-Assignment 'IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY' (Get-Shared 'SAS_GROUP_KEY')
        Format-Assignment 'IOT_DPS_CONNECTION_STRING' (Get-Shared 'DPS_CS')
    )
} else {
    @(
        Format-Assignment 'IOTHUB_CONNECTION_STRING' (Get-Shared 'IOTHUB_CS')
        Format-Assignment 'IOTHUB_EVENTHUB_CONNECTION_STRING' (Get-Shared 'EVENTHUB_CS')
        Format-Assignment 'IOTHUB_EVENTHUB_CONSUMER_GROUP' $ConsumerGroup
        Format-Assignment 'IOT_DPS_ID_SCOPE' (Get-Shared 'ID_SCOPE')
        if ($Mqttv5 -and (Get-Shared 'DPS_HOST')) { Format-Assignment 'IOT_DPS_GLOBAL_ENDPOINT' (Get-Shared 'DPS_HOST') }
        Format-Assignment "IOT_DPS_INDIVIDUAL_REGISTRATION_ID_$DeviceIndex" $RegistrationId
        Format-Assignment "IOT_DPS_INDIVIDUAL_X509_CERTIFICATE_$DeviceIndex" (ConvertTo-B64 $Chain)
        Format-Assignment "IOT_DPS_INDIVIDUAL_X509_KEY_$DeviceIndex" $KeyB64
        if (-not $Mqttv5) { Format-Assignment 'IOT_DPS_SYMM_KEY_GROUP_PRIMARY_KEY' (Get-Shared 'SAS_GROUP_KEY') }
        if (-not $Mqttv5) { Format-Assignment 'IOT_DPS_CONNECTION_STRING' (Get-Shared 'DPS_CS') }
        if (-not $Mqttv5) { Format-Assignment 'IOTHUB_REGISTRY_CONNECTION_STRING' (Get-Shared 'IOTHUB_REGISTRY_CS') }
    )
}
$OutDir = Split-Path -Parent $OutFile
if ($OutDir) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
Set-Content -Path $OutFile -Value $Lines -Encoding utf8NoBOM
if ($Csr) {
    Write-Host "Wrote $OutFile for CSR bootstrap device '$RegistrationId' (device index $DeviceIndex)."
} else {
    Write-Host "Wrote $OutFile for '$RegistrationId' (device index $DeviceIndex, consumer group $ConsumerGroup)."
}
