# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
Claims or releases one e2e device on a long-lived, shared IoT Hub + DPS.

.DESCRIPTION
Acquire: reads the single IoT Hub and DPS in -ResourceGroup through the signed-in Azure CLI, creates
DPS x509 individual enrollment -RegistrationId with a new self-signed certificate, and writes the
test-config script that ci-c-e2e.yml sources (same variables the provisioning action emits, for
-DeviceIndex only). Keys are masked in GitHub Actions logs.

Release: deletes the enrollment, its registration record and its IoT Hub device. Best effort; a
missing item is not an error.

Nothing here creates or deletes Azure resources: the shared environment is set up once, by hand.
#>
param(
    [Parameter(Mandatory)][ValidateSet('Acquire', 'Release')][string]$Action,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9._()-]{1,90}$')][string]$ResourceGroup,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,127}$')][string]$RegistrationId,
    [ValidateRange(0, 99)][int]$DeviceIndex = 0,
    [ValidatePattern('^[A-Za-z0-9$._-]{1,50}$')][string]$ConsumerGroup = '$Default',
    [string]$OutFile = 'test_config/set_test_env_vars.ps1'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$DpsApiVersion = '2021-10-01'
$HubApiVersion = '2021-04-12'

function Invoke-Arm([string]$Method, [string]$Path) {
    $Json = az rest --method $Method --url "/subscriptions/{subscriptionId}/resourceGroups/$ResourceGroup/providers/Microsoft.Devices/$Path" --only-show-errors
    if ($LASTEXITCODE -ne 0) { throw "ARM $Method $Path failed ($LASTEXITCODE)." }
    return ($Json -join "`n") | ConvertFrom-Json
}

function Get-Single([string]$Type, [string]$ApiVersion) {
    $Items = @((Invoke-Arm GET "${Type}?api-version=$ApiVersion").value)
    if ($Items.Count -ne 1) { throw "Expected exactly one $Type in '$ResourceGroup'; found $($Items.Count)." }
    return $Items[0]
}

function Hide-Secret([string]$Value) {
    if ($env:GITHUB_ACTIONS -eq 'true') { Write-Host "::add-mask::$Value" }
}

function New-SasToken([string]$ServiceHost, [string]$KeyName, [string]$Key) {
    $Expiry = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 3600
    $Resource = [uri]::EscapeDataString($ServiceHost)
    $Hmac = [System.Security.Cryptography.HMACSHA256]::new([Convert]::FromBase64String($Key))
    try {
        $Sig = [Convert]::ToBase64String($Hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("$Resource`n$Expiry")))
    } finally {
        $Hmac.Dispose()
    }
    return "SharedAccessSignature sr=$Resource&sig=$([uri]::EscapeDataString($Sig))&se=$Expiry&skn=$KeyName"
}

# Invokes a data-plane REST call. Returns $false on 404 when -AllowNotFound, throws on any other failure.
function Invoke-DataPlane([string]$Method, [string]$Uri, [string]$Sas, [string]$Body = $null, [switch]$AllowNotFound) {
    $Headers = @{ Authorization = $Sas }
    if ($Method -eq 'DELETE') { $Headers['If-Match'] = '*' }
    $Params = @{ Method = $Method; Uri = $Uri; Headers = $Headers; SkipHttpErrorCheck = $true; StatusCodeVariable = 'Status' }
    if ($Body) { $Params['Body'] = $Body; $Params['ContentType'] = 'application/json' }
    $Response = Invoke-RestMethod @Params
    if ($Status -ge 200 -and $Status -lt 300) { return $true }
    if ($AllowNotFound -and $Status -eq 404) { return $false }
    throw "$Method $Uri failed: HTTP $Status $($Response | ConvertTo-Json -Compress -Depth 5)"
}

$Hub = Get-Single 'IotHubs' '2023-06-30'
$HubHost = $Hub.properties.hostName
$HubKey = (Invoke-Arm POST "IotHubs/$($Hub.name)/IoTHubKeys/iothubowner/listkeys?api-version=2023-06-30").primaryKey
Hide-Secret $HubKey

$Dps = Get-Single 'provisioningServices' '2022-02-05'
$DpsHost = $Dps.properties.serviceOperationsHostName
$DpsKey = (Invoke-Arm POST "provisioningServices/$($Dps.name)/keys/provisioningserviceowner/listkeys?api-version=2022-02-05").primaryKey
Hide-Secret $DpsKey

$HubSas = New-SasToken $HubHost 'iothubowner' $HubKey
$DpsSas = New-SasToken $DpsHost 'provisioningserviceowner' $DpsKey
$Id = [uri]::EscapeDataString($RegistrationId)

if ($Action -eq 'Release') {
    $Failed = 0
    foreach ($Uri in @(
            "https://$DpsHost/enrollments/${Id}?api-version=$DpsApiVersion",
            "https://$DpsHost/registrations/${Id}?api-version=$DpsApiVersion",
            "https://$HubHost/devices/${Id}?api-version=$HubApiVersion")) {
        $Sas = if ($Uri.StartsWith("https://$HubHost/")) { $HubSas } else { $DpsSas }
        try {
            $Deleted = Invoke-DataPlane DELETE $Uri $Sas -AllowNotFound
            Write-Host "$(if ($Deleted) { 'Deleted' } else { 'Not found' }): $Uri"
        } catch {
            Write-Warning $_.Exception.Message
            $Failed++
        }
    }
    if ($Failed -gt 0) { throw "Release of '$RegistrationId' left $Failed item(s) behind." }
    return
}

# Device identity: self-signed leaf, as the provisioning action creates for its enrollments.
$Rsa = [System.Security.Cryptography.RSA]::Create(2048)
$Request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
    "CN=$RegistrationId", $Rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
    [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $false))
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor
        [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyEncipherment, $false))
$Eku = [System.Security.Cryptography.OidCollection]::new()
[void]$Eku.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.2'))
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($Eku, $false))
$Request.CertificateExtensions.Add(
    [System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($Request.PublicKey, $false))
$Now = [DateTimeOffset]::UtcNow
$Cert = $Request.CreateSelfSigned($Now.AddMinutes(-10), $Now.AddDays(2))

$Enrollment = @{
    registrationId     = $RegistrationId
    provisioningStatus = 'enabled'
    attestation        = @{
        type = 'x509'
        x509 = @{ clientCertificates = @{ primary = @{ certificate = [Convert]::ToBase64String($Cert.RawData) } } }
    }
} | ConvertTo-Json -Compress -Depth 10

for ($Attempt = 1; ; $Attempt++) {
    try {
        [void](Invoke-DataPlane PUT "https://$DpsHost/enrollments/${Id}?api-version=$DpsApiVersion" $DpsSas $Enrollment)
        break
    } catch {
        if ($Attempt -ge 3) { throw }
        Write-Host "Enrollment create attempt $Attempt failed: $($_.Exception.Message). Retrying."
        Start-Sleep -Seconds (5 * $Attempt)
    }
}
Write-Host "Created DPS enrollment '$RegistrationId' on $($Dps.name)."

function ConvertTo-B64([string]$Text) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text)) }
function Format-Assignment([string]$Name, [string]$Value) { "`$env:$Name = '$($Value.Replace("'", "''"))'" }

$Events = $Hub.properties.eventHubEndpoints.events
$Lines = @(
    Format-Assignment 'IOTHUB_CONNECTION_STRING' "HostName=$HubHost;SharedAccessKeyName=iothubowner;SharedAccessKey=$HubKey"
    Format-Assignment 'IOTHUB_EVENTHUB_CONNECTION_STRING' "Endpoint=$($Events.endpoint);SharedAccessKeyName=iothubowner;SharedAccessKey=$HubKey;EntityPath=$($Events.path)"
    Format-Assignment 'IOTHUB_EVENTHUB_LISTEN_NAME' $Events.path
    Format-Assignment 'IOTHUB_EVENTHUB_PARTITION_COUNT' "$($Events.partitionCount)"
    Format-Assignment 'IOTHUB_EVENTHUB_CONSUMER_GROUP' $ConsumerGroup
    Format-Assignment 'IOT_DPS_ID_SCOPE' $Dps.properties.idScope
    Format-Assignment 'IOT_DPS_GLOBAL_ENDPOINT' $Dps.properties.deviceProvisioningHostName
    Format-Assignment "IOT_DPS_INDIVIDUAL_REGISTRATION_ID_$DeviceIndex" $RegistrationId
    Format-Assignment "IOT_DPS_INDIVIDUAL_X509_CERTIFICATE_$DeviceIndex" (ConvertTo-B64 $Cert.ExportCertificatePem())
    Format-Assignment "IOT_DPS_INDIVIDUAL_X509_KEY_$DeviceIndex" (ConvertTo-B64 $Rsa.ExportRSAPrivateKeyPem())
)
$OutDir = Split-Path -Parent $OutFile
if ($OutDir) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
Set-Content -Path $OutFile -Value $Lines -Encoding utf8NoBOM
Write-Host "Wrote $OutFile (device index $DeviceIndex, consumer group $ConsumerGroup)."
