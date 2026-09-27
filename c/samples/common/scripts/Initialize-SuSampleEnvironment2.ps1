#Requires -Version 7.0
<#
.SYNOPSIS
    Parallelized variant of Initialize-SuSampleEnvironment.ps1: provisions (or
    reuses) an IoT Hub / DPS / Device Update environment and sets the environment
    variables the samples read.

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

    Same end result as Initialize-SuSampleEnvironment.ps1, but the independent
    Azure operations are fanned out across background thread jobs so the slow
    creates (IoT Hub, DPS, Storage) run concurrently instead of one after the
    other. Every log line is prefixed with the task it came from, e.g.

        [hub    ] Creating IoT Hub su-sim-hub-ab12cd (S1, TLS 1.2)...
        [dps    ] Creating Device Provisioning Service su-sim-dps-ab12cd...
        [storage] Creating storage account susimab12cd...

    Dependency order is preserved: resources that depend on others only start
    once their inputs are ready.

    The resource group is the identity of an environment:

      * Pass -ResourceGroup to REUSE an existing environment (discovered, then read
        sequentially). Fails if the group is missing or incomplete.
      * Omit -ResourceGroup to CREATE a brand-new environment from scratch, fanning
        the independent creates out across thread jobs.

    On success the sample's environment variables are set in the CURRENT
    PowerShell session, a sourceable su-sample-env.sh is written, and a short
    summary is printed.

.EXAMPLE
    ./Initialize-SuSampleEnvironment2.ps1

.EXAMPLE
    ./Initialize-SuSampleEnvironment2.ps1 -ResourceGroup su-sim-rg-72351e
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

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Labeled logging
# ---------------------------------------------------------------------------

# Stable color per task so the eye can follow a stream of interleaved lines.
$script:LabelColors = @{
    core    = "White"
    cert    = "Blue"
    rg      = "Cyan"
    hub     = "Green"
    dps     = "Yellow"
    storage = "Magenta"
    link    = "DarkCyan"
    enroll  = "DarkYellow"
    du      = "DarkGreen"
}

function Write-Label {
    param([string]$Label, [string]$Message)
    $color = $script:LabelColors[$Label]
    if (-not $color) { $color = "Gray" }
    Write-Host ("[{0,-7}] " -f $Label) -ForegroundColor $color -NoNewline
    Write-Host $Message
}

# Poll a set of running thread jobs, streaming each emitted line prefixed with
# the job's name, then surface any failures and clean up. Throws if any failed.
function Wait-LabeledJobs {
    param([System.Management.Automation.Job[]]$Jobs)

    # A job is "done" only once it reaches a terminal state. Start-ThreadJob
    # returns jobs in the NotStarted state that flip to Running a moment later,
    # so we must NOT treat "no job is Running yet" as completion -- doing so
    # races the jobs and force-removes them before their script blocks execute.
    $terminalStates = @("Completed", "Failed", "Stopped")
    while ($true) {
        foreach ($j in $Jobs) {
            foreach ($line in (Receive-Job -Job $j -ErrorAction SilentlyContinue)) {
                Write-Label $j.Name ([string]$line)
            }
        }
        $pending = @($Jobs | Where-Object { $_.State -notin $terminalStates })
        if (-not $pending) { break }
        Start-Sleep -Milliseconds 200
    }

    # Drain any output emitted between the last poll and the jobs finishing.
    foreach ($j in $Jobs) {
        foreach ($line in (Receive-Job -Job $j -ErrorAction SilentlyContinue)) {
            Write-Label $j.Name ([string]$line)
        }
    }

    $failed = @($Jobs | Where-Object State -eq "Failed")
    foreach ($j in $failed) {
        $reason = $j.ChildJobs[0].JobStateInfo.Reason
        if (-not $reason -and $j.ChildJobs[0].Error.Count -gt 0) {
            $reason = $j.ChildJobs[0].Error[0].Exception
        }
        Write-Label $j.Name ("FAILED: " + $(if ($reason) { $reason.Message } else { "unknown error" }))
    }
    $Jobs | Remove-Job -Force
    if ($failed) {
        throw "Provisioning failed in: $($failed.Name -join ', ')"
    }
}

# ---------------------------------------------------------------------------
# Preflight + naming
# ---------------------------------------------------------------------------

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
Write-Label core "Checking for the azure-iot Azure CLI extension"
$aziotExt = az extension show --name azure-iot --query name -o tsv 2>$null
if (-not $aziotExt) {
    Write-Label core "azure-iot extension not found; installing it"
    az extension add --name azure-iot | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install the azure-iot Azure CLI extension. Install it manually with 'az extension add --name azure-iot' and re-run."
    }
}

# Decide between REUSE and CREATE based on whether a resource group was given.
$reuse = [bool]$ResourceGroup

$DiagSettingName = "su-sim-conn-diag"
$certPath = Join-Path $PWD "su-sim-device-cert.pem"
$keyPath  = Join-Path $PWD "su-sim-device-key.pem"
$trustedCa = "/etc/ssl/certs/ca-certificates.crt"

if ($reuse) {
    # -----------------------------------------------------------------------
    # REUSE: discover the existing environment in the group (reads only). Fail
    # if the group is missing or does not hold a complete sample environment.
    # -----------------------------------------------------------------------
    Write-Label core "Reusing the existing environment in resource group $ResourceGroup"
    if ((az group exists --name $ResourceGroup) -ne 'true') {
        throw "Resource group '$ResourceGroup' does not exist. Omit -ResourceGroup to create a new environment from scratch."
    }

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

    if (-not $InstanceName) { $InstanceName = az iot du instance list --account $AccountName --resource-group $ResourceGroup --query "[0].name" -o tsv }
    $InstanceName = Get-Required "Device Update instance" $InstanceName

    if (-not $LogWorkspace) { $LogWorkspace = az monitor log-analytics workspace list --resource-group $ResourceGroup --query "[0].name" -o tsv }

    $hubId     = az iot hub show --name $IotHubName --resource-group $ResourceGroup --query id -o tsv
    $storageId = az storage account show --name $StorageAccount --resource-group $ResourceGroup --query id -o tsv
    $idScope   = az iot dps show --name $DpsName --resource-group $ResourceGroup --query properties.idScope -o tsv

    Write-Label core "IoT Hub $IotHubName / DPS $DpsName / Storage $StorageAccount / DU $AccountName/$InstanceName"

    # Device certificate + enrollment. Reuse the local PEMs when they match an
    # existing enrollment; otherwise regenerate and (create or update) it.
    $enrollmentExists = az iot dps enrollment show --dps-name $DpsName --resource-group $ResourceGroup --enrollment-id $DeviceId --query registrationId -o tsv 2>$null
    if ($enrollmentExists -and (Test-Path $certPath) -and (Test-Path $keyPath)) {
        Write-Label cert "Reusing existing device certificate $certPath"
    } else {
        Write-Label cert "Generating self-signed device certificate (CN=$DeviceId)"
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
            Write-Label enroll "Updating DPS enrollment $DeviceId with the new certificate"
            az iot dps enrollment update --dps-name $DpsName --resource-group $ResourceGroup `
                --enrollment-id $DeviceId --certificate-path $certPath | Out-Null
        } else {
            Write-Label enroll "Creating DPS X.509 individual enrollment $DeviceId"
            az iot dps enrollment create --dps-name $DpsName --resource-group $ResourceGroup `
                --enrollment-id $DeviceId --attestation-type x509 --certificate-path $certPath | Out-Null
        }
    }
}
else {
    # -----------------------------------------------------------------------
    # CREATE: provision a brand-new environment, fanning the independent creates
    # out across thread jobs.
    # -----------------------------------------------------------------------
    $suffix = [System.Guid]::NewGuid().ToString("N").Substring(0, 6)
    $ResourceGroup  = "su-sim-rg-$suffix"
    if (-not $IotHubName)     { $IotHubName     = "su-sim-hub-$suffix" }
    if (-not $DpsName)        { $DpsName        = "su-sim-dps-$suffix" }
    if (-not $AccountName)    { $AccountName    = "su-sim-acct-$suffix" }
    if (-not $InstanceName)   { $InstanceName   = "su-sim-inst-$suffix" }
    if (-not $StorageAccount) { $StorageAccount = "susim$suffix" }
    $LogWorkspace = "su-sim-logs-$suffix"

    # Shared bag the thread jobs publish their lookups into (same process, so a
    # synchronized hashtable is safe to write from multiple jobs).
    $shared = [hashtable]::Synchronized(@{})

# ---------------------------------------------------------------------------
# Resource group (everything else needs it) — sequential, fast.
# ---------------------------------------------------------------------------

Write-Label rg "Creating resource group $ResourceGroup ($Location)"
az group create --name $ResourceGroup --location $Location | Out-Null
if ($LASTEXITCODE -ne 0) { throw "resource group create failed ($LASTEXITCODE)" }

# ---------------------------------------------------------------------------
# Device certificate — pure-local .NET, no Azure dependency. Do it now so it is
# ready by the time the (parallel) DPS enrollment needs it.
# ---------------------------------------------------------------------------

Write-Label cert "Generating self-signed device certificate (CN=$DeviceId)"
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
Write-Label cert "Certificate written to $certPath"

# ---------------------------------------------------------------------------
# Round 1 (parallel): IoT Hub, DPS, and Storage are mutually independent.
# Each also publishes the resource id / id-scope the later steps consume.
# ---------------------------------------------------------------------------

Write-Label core "Creating IoT Hub, DPS, and Storage in parallel..."

$hubJob = Start-ThreadJob -Name hub -ArgumentList $IotHubName, $ResourceGroup, $Location, $shared -ScriptBlock {
    param($Name, $Rg, $Loc, $Shared)
    "Creating IoT Hub $Name (S1, TLS 1.2)..."
    az iot hub create --name $Name --resource-group $Rg --location $Loc --sku S1 --mintls 1.2 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "iot hub create failed ($LASTEXITCODE)" }
    $id = az iot hub show --name $Name --resource-group $Rg --query id -o tsv
    if ($LASTEXITCODE -ne 0) { throw "iot hub show failed ($LASTEXITCODE)" }
    $Shared['hubId'] = $id
    "IoT Hub ready."
}

$dpsJob = Start-ThreadJob -Name dps -ArgumentList $DpsName, $ResourceGroup, $Location, $shared -ScriptBlock {
    param($Name, $Rg, $Loc, $Shared)
    "Creating Device Provisioning Service $Name..."
    az iot dps create --name $Name --resource-group $Rg --location $Loc | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "iot dps create failed ($LASTEXITCODE)" }
    "Reading DPS ID scope..."
    $scope = az iot dps show --name $Name --resource-group $Rg --query properties.idScope -o tsv
    if ($LASTEXITCODE -ne 0) { throw "iot dps show failed ($LASTEXITCODE)" }
    $Shared['idScope'] = $scope
    "DPS ready (ID scope $scope)."
}

$storageJob = Start-ThreadJob -Name storage -ArgumentList $StorageAccount, $ResourceGroup, $Location, $ContainerName, $shared -ScriptBlock {
    param($Name, $Rg, $Loc, $Container, $Shared)
    "Creating storage account $Name (Standard_LRS)..."
    az storage account create --name $Name --resource-group $Rg --location $Loc --sku Standard_LRS | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "storage account create failed ($LASTEXITCODE)" }
    "Creating blob container $Container..."
    az storage container create --account-name $Name --name $Container --auth-mode login | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "storage container create failed ($LASTEXITCODE)" }
    $id = az storage account show --name $Name --resource-group $Rg --query id -o tsv
    if ($LASTEXITCODE -ne 0) { throw "storage account show failed ($LASTEXITCODE)" }
    $Shared['storageId'] = $id
    "Storage ready."
}

Wait-LabeledJobs -Jobs $hubJob, $dpsJob, $storageJob

$hubId     = $shared['hubId']
$storageId = $shared['storageId']
$idScope   = $shared['idScope']

# ---------------------------------------------------------------------------
# Round 2 (parallel): everything that only needed Round 1's outputs.
#   link   : DPS<->Hub link        (needs Hub + DPS)
#   enroll : DPS X.509 enrollment  (needs DPS + cert)
#   du     : Device Update acct+instance (needs storageId, then hubId)
# ---------------------------------------------------------------------------

Write-Label core "Linking, enrolling, and creating Device Update in parallel..."

$linkJob = Start-ThreadJob -Name link -ArgumentList $DpsName, $ResourceGroup, $IotHubName -ScriptBlock {
    param($Dps, $Rg, $Hub)
    "Linking IoT Hub $Hub to DPS $Dps..."
    az iot dps linked-hub create --dps-name $Dps --resource-group $Rg --hub-name $Hub | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "dps linked-hub create failed ($LASTEXITCODE)" }
    "Link ready."
}

$enrollJob = Start-ThreadJob -Name enroll -ArgumentList $DpsName, $ResourceGroup, $DeviceId, $certPath -ScriptBlock {
    param($Dps, $Rg, $Device, $CertPath)
    "Creating DPS X.509 individual enrollment $Device..."
    az iot dps enrollment create --dps-name $Dps --resource-group $Rg `
        --enrollment-id $Device --attestation-type x509 --certificate-path $CertPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "dps enrollment create failed ($LASTEXITCODE)" }
    "Enrollment ready."
}

$duJob = Start-ThreadJob -Name du -ArgumentList $AccountName, $InstanceName, $ResourceGroup, $Location, $storageId, $hubId, $IotHubName, $LogWorkspace, $DiagSettingName -ScriptBlock {
    param($Acct, $Inst, $Rg, $Loc, $StorageId, $HubId, $Hub, $LogWs, $DiagName)
    "Creating Device Update account $Acct (system identity + storage RBAC)..."
    az iot du account create --account $Acct --resource-group $Rg --location $Loc `
        --assign-identity [system] --scopes $StorageId --role "Storage Blob Data Contributor" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "iot du account create failed ($LASTEXITCODE)" }
    "Creating Device Update instance $Inst (linked to $Hub)..."
    az iot du instance create --account $Acct --instance $Inst --resource-group $Rg --iothub-ids $HubId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "iot du instance create failed ($LASTEXITCODE)" }
    "Granting current user 'Device Update Administrator' on $Acct..."
    $accountId = az iot du account show --account $Acct --resource-group $Rg --query id -o tsv
    $currentUserId = az ad signed-in-user show --query id -o tsv
    az role assignment create --assignee $currentUserId --role "Device Update Administrator" --scope $accountId | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "device update role assignment failed ($LASTEXITCODE)" }
    "Enabling IoT Hub connection diagnostics ($DiagName -> $LogWs)..."
    az monitor log-analytics workspace create --resource-group $Rg --workspace-name $LogWs --location $Loc | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "log-analytics workspace create failed ($LASTEXITCODE)" }
    $workspaceId = az monitor log-analytics workspace show --resource-group $Rg --workspace-name $LogWs --query id -o tsv
    az monitor diagnostic-settings create --name $DiagName --resource $HubId `
        --workspace $workspaceId --logs '[{\"category\":\"Connections\",\"enabled\":true}]' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "diagnostic-settings create failed ($LASTEXITCODE)" }
    "Device Update ready."
}

    Wait-LabeledJobs -Jobs $linkJob, $enrollJob, $duJob
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
Write-Label core "Ensuring the Azure Device Update service principal can read the hub"
$suSpId = az ad sp list --display-name "Azure Device Update" --filter "displayName eq 'Azure Device Update'" --query "[0].id" -o tsv 2>$null
if ($suSpId) {
    az role assignment create --assignee-object-id $suSpId --assignee-principal-type ServicePrincipal `
        --role "IoT Hub Data Contributor" --scope $hubId --only-show-errors | Out-Null
} else {
    Write-Warning "Could not find the 'Azure Device Update' service principal; device grouping may not work until 'IoT Hub Data Contributor' is granted to it on the hub."
}

# ---------------------------------------------------------------------------
# Publish environment variables + summary
# ---------------------------------------------------------------------------

# Environment variables the sample reads.
$env:AZ_IOT_DPS_ID_SCOPE        = $idScope
$env:AZ_IOT_DPS_REGISTRATION_ID = $DeviceId
$env:AZ_IOT_CLIENT_CERT         = $certPath
$env:AZ_IOT_CLIENT_KEY          = $keyPath
$env:AZ_IOT_TRUSTED_CA          = $trustedCa

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
