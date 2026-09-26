#Requires -Version 7.0
<#
.SYNOPSIS
    Provisions (or reuses) an IoT Hub / DPS / Device Update environment and sets
    the environment variables the samples read.

.DESCRIPTION
    IMPORTANT - which Device Update model this creates.

    The Device Update resources created here - 'az iot du account create' and
    'az iot du instance create --iothub-ids' - belong to the IoT-Hub-based Device
    Update model. The software updates samples in this repository implement the DPS-fronted
    model, which has no Device Update accounts: it needs a
    Microsoft.DeviceUpdate/updateInstances resource and a
    Microsoft.DeviceRegistry/namespaces linked to it, with the DPS linked to that
    namespace through a managed identity. This script does NOT create those, and
    New-SuSampleDeployment.ps1 cannot deliver an update to those samples.

    What this script produces that the samples do use: the resource group, the
    DPS, the device certificate and its X.509 enrollment, the storage account,
    and the AZ_IOT_* environment variables.

    See samples/su/pc/README.md for what the samples need from a Device
    Update environment.

    The resource group is the identity of an environment:

      * Pass -ResourceGroup to REUSE an existing environment. The script verifies
        the group exists and discovers the IoT Hub, DPS, storage account and
        Device Update account/instance inside it. If the group is missing or does
        not contain a complete software updates sample environment, the script fails (it never
        creates resources in this mode).

      * Omit -ResourceGroup to CREATE a brand-new environment from scratch. A fresh
        resource group and all companion resources are provisioned under generated
        names.

    On success the sample's environment variables are set in the CURRENT PowerShell
    session and a short summary is printed.

.EXAMPLE
    ./Initialize-SuSampleEnvironment.ps1

.EXAMPLE
    ./Initialize-SuSampleEnvironment.ps1 -ResourceGroup su-sim-rg-72351e
#>
param(
    [string]$IotHubName,
    [string]$ResourceGroup,
    [string]$Location = "eastus",
    [string]$DpsName,
    [string]$AccountName,
    [string]$InstanceName,
    [string]$StorageAccount,
    [string]$ContainerName = "suimports",
    [string]$DeviceId = "su-sim-device",
    [string]$GroupId = "su-sim-devices"
)

Write-Warning @'
This script provisions the IoT-Hub-based Device Update model (account + instance
linked to a hub). The software updates samples in this repository implement the DPS-fronted
model, which has no accounts: it needs a Microsoft.DeviceUpdate/updateInstances
resource and a Microsoft.DeviceRegistry/namespaces linked to it, with the DPS
linked to that namespace. Those are NOT created here, and the samples will not be
offered an update by this environment.

Still produced and usable: resource group, DPS, device certificate + X.509
enrollment, storage, and the AZ_IOT_* environment variables.

See samples/su/pc/README.md for what the samples need from a Device Update
environment.
'@

# The az CLI 'iot' commands live in the azure-iot extension. Make sure it is
# installed before any of them run.
Write-Host "==> Checking for the azure-iot Azure CLI extension"
$aziotExt = az extension show --name azure-iot --query name -o tsv 2>$null
if (-not $aziotExt) {
    Write-Host "    azure-iot extension not found; installing it"
    az extension add --name azure-iot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install the azure-iot Azure CLI extension. Install it manually with 'az extension add --name azure-iot' and re-run."
    }
}

# Decide between REUSE and CREATE based on whether a resource group was given.
#   -ResourceGroup supplied -> reuse the existing environment in that group;
#                              fail if the group or its resources are missing.
#   -ResourceGroup omitted  -> create a brand-new environment from scratch.
$reuse = [bool]$ResourceGroup

# Connection diagnostics setting name (constant; the workspace name varies).
$DiagSettingName = "su-sim-conn-diag"

$certPath = Join-Path $PWD "su-sim-device-cert.pem"
$keyPath  = Join-Path $PWD "su-sim-device-key.pem"
$trustedCa = "/etc/ssl/certs/ca-certificates.crt"

if ($reuse) {
    Write-Host "==> Reusing the existing environment in resource group $ResourceGroup"
    if ((az group exists --name $ResourceGroup) -ne 'true') {
        throw "Resource group '$ResourceGroup' does not exist. Omit -ResourceGroup to create a new environment from scratch."
    }

    # Discover the resources that live in the group. Each required one that is
    # missing means this group is not a complete software updates sample environment, so fail
    # rather than silently provisioning a partial one.
    function Get-Required([string]$description, [string]$name) {
        if (-not $name) {
            throw "Resource group '$ResourceGroup' has no $description; it does not look like a complete software updates sample environment. Omit -ResourceGroup to create one from scratch."
        }
        return $name
    }

    if (-not $IotHubName)     { $IotHubName     = az iot hub list --resource-group $ResourceGroup --query "[0].name" -o tsv }
    if (-not $DpsName)        { $DpsName        = az iot dps list --resource-group $ResourceGroup --query "[0].name" -o tsv }
    if (-not $StorageAccount) { $StorageAccount = az storage account list --resource-group $ResourceGroup --query "[0].name" -o tsv }
    if (-not $AccountName)    { $AccountName    = az iot du account list --resource-group $ResourceGroup --query "[0].name" -o tsv }

    $IotHubName     = Get-Required "IoT Hub"                     $IotHubName
    $DpsName        = Get-Required "Device Provisioning Service" $DpsName
    $StorageAccount = Get-Required "storage account"            $StorageAccount
    $AccountName    = Get-Required "Device Update account"      $AccountName

    if (-not $InstanceName)  { $InstanceName  = az iot du instance list --account $AccountName --resource-group $ResourceGroup --query "[0].name" -o tsv }
    $InstanceName = Get-Required "Device Update instance" $InstanceName

    # The log workspace is only used for diagnostics; reuse it if present but do
    # not fail when it is absent.
    if (-not $LogWorkspace) { $LogWorkspace = az monitor log-analytics workspace list --resource-group $ResourceGroup --query "[0].name" -o tsv }

    $hubId     = az iot hub show --name $IotHubName --resource-group $ResourceGroup --query id -o tsv
    $storageId = az storage account show --name $StorageAccount --resource-group $ResourceGroup --query id -o tsv

    Write-Host "    IoT Hub        : $IotHubName"
    Write-Host "    DPS            : $DpsName"
    Write-Host "    Storage        : $StorageAccount"
    Write-Host "    Device Update  : $AccountName / instance $InstanceName"
    if ($LogWorkspace) { Write-Host "    Log workspace  : $LogWorkspace" }
}
else {
    $suffix = [System.Guid]::NewGuid().ToString("N").Substring(0, 6)
    $ResourceGroup  = "su-sim-rg-$suffix"
    if (-not $IotHubName)     { $IotHubName     = "su-sim-hub-$suffix" }
    if (-not $DpsName)        { $DpsName        = "su-sim-dps-$suffix" }
    if (-not $AccountName)    { $AccountName    = "su-sim-acct-$suffix" }
    if (-not $InstanceName)   { $InstanceName   = "su-sim-inst-$suffix" }
    if (-not $StorageAccount) { $StorageAccount = "susim$suffix" }
    $LogWorkspace = "su-sim-logs-$suffix"

    Write-Host "==> Creating resource group $ResourceGroup ($Location)"
    az group create --name $ResourceGroup --location $Location | Out-Null

    Write-Host "==> Creating IoT Hub $IotHubName"
    az iot hub create --name $IotHubName --resource-group $ResourceGroup --location $Location --sku S1 --mintls 1.2 | Out-Null
    $hubId = az iot hub show --name $IotHubName --resource-group $ResourceGroup --query id -o tsv

    Write-Host "==> Creating Device Provisioning Service $DpsName"
    az iot dps create --name $DpsName --resource-group $ResourceGroup --location $Location | Out-Null

    Write-Host "==> Linking IoT Hub $IotHubName to DPS $DpsName"
    az iot dps linked-hub create --dps-name $DpsName --resource-group $ResourceGroup --hub-name $IotHubName | Out-Null

    Write-Host "==> Creating storage account $StorageAccount"
    az storage account create --name $StorageAccount --resource-group $ResourceGroup --location $Location --sku Standard_LRS | Out-Null
    $storageId = az storage account show --name $StorageAccount --resource-group $ResourceGroup --query id -o tsv

    Write-Host "==> Creating blob container $ContainerName"
    az storage container create --account-name $StorageAccount --name $ContainerName --auth-mode login | Out-Null

    Write-Host "==> Creating Device Update account $AccountName (system identity + storage access)"
    az iot du account create --account $AccountName --resource-group $ResourceGroup --location $Location `
        --assign-identity [system] --scopes $storageId --role "Storage Blob Data Contributor" | Out-Null

    Write-Host "==> Creating Device Update instance $InstanceName (linked to $IotHubName)"
    az iot du instance create --account $AccountName --instance $InstanceName --resource-group $ResourceGroup --iothub-ids $hubId | Out-Null

    Write-Host "==> Granting current user 'Device Update Administrator' on $AccountName"
    $accountId = az iot du account show --account $AccountName --resource-group $ResourceGroup --query id -o tsv
    $currentUserId = az ad signed-in-user show --query id -o tsv
    az role assignment create --assignee $currentUserId --role "Device Update Administrator" --scope $accountId | Out-Null

    Write-Host "==> Enabling IoT Hub connection diagnostics ($DiagSettingName -> $LogWorkspace)"
    az monitor log-analytics workspace create --resource-group $ResourceGroup --workspace-name $LogWorkspace --location $Location | Out-Null
    $workspaceId = az monitor log-analytics workspace show --resource-group $ResourceGroup --workspace-name $LogWorkspace --query id -o tsv
    az monitor diagnostic-settings create --name $DiagSettingName --resource $hubId `
        --workspace $workspaceId --logs '[{\"category\":\"Connections\",\"enabled\":true}]' | Out-Null
}

# Software updates classifies tagged devices into groups by reading the IoT Hub's device
# twins, which requires the first-party 'Azure Device Update' service principal
# to have 'IoT Hub Data Contributor' on the hub. Portal linking creates this role
# assignment for you, but CLI/ARM linking (az iot du instance create --iothub-ids,
# used above) records the instance<->hub association WITHOUT granting the role, so
# we must do it ourselves. Without it the device group never materializes
# (ServicePrincipalMissingPermissions / IotHubUnauthorized) and 'deployment
# create' 404s. The grant is idempotent, so it is safe in both reuse and create
# modes (and a no-op if the portal already set it up).
Write-Host "==> Ensuring the Azure Device Update service principal can read the hub"
$suSpId = az ad sp list --display-name "Azure Device Update" --filter "displayName eq 'Azure Device Update'" --query "[0].id" -o tsv 2>$null
if ($suSpId) {
    az role assignment create --assignee-object-id $suSpId --assignee-principal-type ServicePrincipal `
        --role "IoT Hub Data Contributor" --scope $hubId --only-show-errors | Out-Null
} else {
    Write-Warning "Could not find the 'Azure Device Update' service principal; device grouping may not work until 'IoT Hub Data Contributor' is granted to it on the hub."
}

$enrollmentExists = az iot dps enrollment show --dps-name $DpsName --resource-group $ResourceGroup --enrollment-id $DeviceId --query registrationId -o tsv 2>$null
$certFilesExist = (Test-Path $certPath) -and (Test-Path $keyPath)

if ($enrollmentExists -and $certFilesExist) {
    Write-Host "==> Reusing device certificate + DPS enrollment $DeviceId"
}
else {
    Write-Host "==> Generating self-signed device certificate (CN=$DeviceId)"
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        "CN=$DeviceId", $rsa,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $cert = $req.CreateSelfSigned([System.DateTimeOffset]::UtcNow.AddDays(-1), [System.DateTimeOffset]::UtcNow.AddDays(365))
    $certB64 = [System.Convert]::ToBase64String($cert.RawData, 'InsertLineBreaks')
    $keyB64  = [System.Convert]::ToBase64String($rsa.ExportPkcs8PrivateKey(), 'InsertLineBreaks')
    Set-Content -Path $certPath -Value "-----BEGIN CERTIFICATE-----`n$certB64`n-----END CERTIFICATE-----"
    Set-Content -Path $keyPath  -Value "-----BEGIN PRIVATE KEY-----`n$keyB64`n-----END PRIVATE KEY-----"

    if ($enrollmentExists) {
        Write-Host "==> Updating DPS individual enrollment $DeviceId (X.509) with the new certificate"
        az iot dps enrollment update --dps-name $DpsName --resource-group $ResourceGroup `
            --enrollment-id $DeviceId --certificate-path $certPath | Out-Null
    }
    else {
        Write-Host "==> Creating DPS individual enrollment $DeviceId (X.509)"
        az iot dps enrollment create --dps-name $DpsName --resource-group $ResourceGroup `
            --enrollment-id $DeviceId --attestation-type x509 --certificate-path $certPath | Out-Null
    }
}

Write-Host "==> Reading DPS ID scope"
$idScope = az iot dps show --name $DpsName --resource-group $ResourceGroup --query properties.idScope -o tsv

# Environment variables the sample reads.
$env:AZ_IOT_DPS_ID_SCOPE       = $idScope
$env:AZ_IOT_DPS_REGISTRATION_ID = $DeviceId
$env:AZ_IOT_CLIENT_CERT        = $certPath
$env:AZ_IOT_CLIENT_KEY         = $keyPath
$env:AZ_IOT_TRUSTED_CA         = $trustedCa

# Environment variables the deployment / cleanup scripts read.
$env:AZ_IOT_SU_RESOURCE_GROUP = $ResourceGroup
$env:AZ_IOT_SU_IOTHUB         = $IotHubName
$env:AZ_IOT_SU_ACCOUNT        = $AccountName
$env:AZ_IOT_SU_INSTANCE       = $InstanceName
$env:AZ_IOT_SU_STORAGE        = $StorageAccount
$env:AZ_IOT_SU_CONTAINER      = $ContainerName
$env:AZ_IOT_SU_DEVICE_ID      = $DeviceId
$env:AZ_IOT_SU_GROUP          = $GroupId
$env:AZ_IOT_SU_DIAG_SETTING   = $DiagSettingName
$env:AZ_IOT_SU_LOG_WORKSPACE  = $LogWorkspace

Write-Host ""
Write-Host "Environment ready. The following variables are set in this session:"
Write-Host "  AZ_IOT_DPS_ID_SCOPE        = $idScope"
Write-Host "  AZ_IOT_DPS_REGISTRATION_ID = $DeviceId"
Write-Host "  AZ_IOT_CLIENT_CERT         = $certPath"
Write-Host "  AZ_IOT_CLIENT_KEY          = $keyPath"
Write-Host "  AZ_IOT_TRUSTED_CA          = $trustedCa"
Write-Host ""
Write-Host "  Resource group : $ResourceGroup"
Write-Host "  IoT Hub        : $IotHubName"
Write-Host "  DPS            : $DpsName"
Write-Host "  Device Update account    : $AccountName / instance $InstanceName"
Write-Host "  Storage        : $StorageAccount / container $ContainerName"
Write-Host "  Device id      : $DeviceId   group $GroupId"
Write-Host "  Conn diag      : $DiagSettingName -> workspace $LogWorkspace"
Write-Host ""

# The sample binary often runs somewhere OTHER than this PowerShell session: a
# Linux box, a Docker container, etc. Emit a bash snippet (and a sourceable .sh
# file) that sets the same variables there. The cert/key are referenced relative
# to the container's working directory so the user only needs to copy the two PEM
# files (and optionally this .sh) into it.
$certLeaf = Split-Path -Leaf $certPath
$keyLeaf  = Split-Path -Leaf $keyPath
$bashExports = @"
# Sample environment for the software updates Linux sample.
# Copy su-sim-device-cert.pem + su-sim-device-key.pem into this directory first,
# then: source ./su-sample-env.sh
export AZ_IOT_DPS_ID_SCOPE='$idScope'
export AZ_IOT_DPS_REGISTRATION_ID='$DeviceId'
export AZ_IOT_CLIENT_CERT="`$PWD/$certLeaf"
export AZ_IOT_CLIENT_KEY="`$PWD/$keyLeaf"
export AZ_IOT_TRUSTED_CA='$trustedCa'
"@
$envScriptPath = Join-Path $PWD "su-sample-env.sh"
# Must be LF-only with no BOM: this file is sourced by bash on Linux/in Docker.
# CRLF line endings would append a trailing \r to every value (e.g. the cert
# path becomes '...cert.pem\r'), which then fails to resolve on Linux.
[System.IO.File]::WriteAllText($envScriptPath, ($bashExports -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Running the sample in Linux/Docker? Copy-paste this into the container shell"
Write-Host "(bash), after copying $certLeaf and $keyLeaf into its working directory:"
Write-Host ""
Write-Host $bashExports
Write-Host ""
Write-Host "The same commands were written to $envScriptPath (source it, or 'docker cp' it in)."
Write-Host ""
Write-Host "Next: build and run the sample, then run ./New-SuSampleDeployment.ps1"
