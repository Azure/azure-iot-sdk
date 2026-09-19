#Requires -Version 7.0
<#
.SYNOPSIS
    Imports a simulated update and deploys it through the IoT-Hub-based Device
    Update model.

.DESCRIPTION
    IMPORTANT - which Device Update model this targets.

    This script drives 'az iot du update ...' and 'az iot du device deployment
    ...', which are scoped to a Device Update account + instance: the
    IoT-Hub-based model. The ADU samples in this repository implement the
    DPS-fronted model, which has no accounts and delivers updates as Azure Device
    Registry jobs and runs on the registry namespace. This script cannot deliver
    an update to those samples.

    See samples/adu/pc/README.md for the job/run shape the samples are offered
    updates through.

    Run this AFTER Initialize-AduSampleEnvironment.ps1 and AFTER the device sample
    is running and connected (the device must exist in the hub so its twin can be
    tagged). Resource names are read from the environment variables set by the
    initialize script; override any of them with parameters.

    Steps: build a zero-filled payload + v5 import manifest, stage+import the
    update, tag the device into the group, then create the deployment.

    By default the update version AUTO-BUMPS to the next unused patch (highest
    existing + 1, or 1.0.0 when none exist) so every run pushes a genuinely new
    update and triggers a fresh workflow on the device. Pass -UpdateVersion to
    target a specific version instead (reused if it already exists).

.EXAMPLE
    ./New-AduSampleDeployment.ps1

.EXAMPLE
    ./New-AduSampleDeployment.ps1 -UpdateVersion 2.0.0
#>
param(
    [string]$ResourceGroup = $env:AZ_IOT_ADU_RESOURCE_GROUP,
    [string]$IotHubName    = $env:AZ_IOT_ADU_IOTHUB,
    [string]$AccountName   = $env:AZ_IOT_ADU_ACCOUNT,
    [string]$InstanceName  = $env:AZ_IOT_ADU_INSTANCE,
    [string]$StorageAccount= $env:AZ_IOT_ADU_STORAGE,
    [string]$ContainerName = $env:AZ_IOT_ADU_CONTAINER,
    [string]$DeviceId      = $env:AZ_IOT_ADU_DEVICE_ID,
    [string]$GroupId       = $env:AZ_IOT_ADU_GROUP,
    [string]$UpdateProvider = "Contoso",
    [string]$UpdateName     = "ADU-Sim",
    [string]$UpdateVersion  = ""
)

Write-Warning @'
This script targets the IoT-Hub-based Device Update model (account + instance).
The ADU samples in this repository implement the DPS-fronted model, which
delivers updates as Azure Device Registry jobs and runs on the registry
namespace. An update imported and deployed by this script will not be offered to
those samples.

See samples/adu/pc/README.md.
'@

$payloadPath  = Join-Path $PWD "adu-sim-payload.bin"
$manifestPath = Join-Path $PWD "adu-sim-manifest.importmanifest.json"
$deploymentId = "adu-sim-deploy-$(Get-Date -Format yyyyMMddHHmmss)"
$startTime    = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

# Resolve the update version. ADU updates are immutable, so re-importing the same
# provider/name/version fails with UpdateAlreadyExists and re-deploying a version
# the device already has installed does not trigger a new workflow. By default we
# therefore AUTO-BUMP to the next unused patch version (highest existing + 1, or
# 1.0.0 when none exist) so every run pushes a genuinely new update. Pass an
# explicit -UpdateVersion to target a specific one instead (it is reused if it
# already exists).
$updateAlreadyImported = $false
if ($UpdateVersion) {
    Write-Host "==> Checking whether update $UpdateProvider/$UpdateName/$UpdateVersion already exists"
    az iot du update show `
        --account $AccountName --instance $InstanceName `
        --update-provider $UpdateProvider --update-name $UpdateName --update-version $UpdateVersion `
        --only-show-errors 2>$null | Out-Null
    $updateAlreadyImported = ($LASTEXITCODE -eq 0)
} else {
    Write-Host "==> Determining the next update version for $UpdateProvider/$UpdateName"
    $existing = az iot du update list `
        --account $AccountName --instance $InstanceName `
        --update-provider $UpdateProvider --update-name $UpdateName `
        --only-show-errors 2>$null | ConvertFrom-Json

    # The list payload differs across CLI versions: it may be bare version
    # strings, or objects carrying a version / updateId.version field. Normalize.
    $versions = @()
    foreach ($item in @($existing)) {
        $v = if ($item -is [string]) { $item }
             elseif ($item.version)  { $item.version }
             elseif ($item.updateId.version) { $item.updateId.version }
             else { $null }
        $parsed = $null
        if ($v -and [version]::TryParse([string]$v, [ref]$parsed)) { $versions += $parsed }
    }

    if ($versions.Count -gt 0) {
        $highest = ($versions | Sort-Object)[-1]
        $UpdateVersion = "{0}.{1}.{2}" -f $highest.Major, $highest.Minor, ([Math]::Max($highest.Build, 0) + 1)
    } else {
        $UpdateVersion = "1.0.0"
    }
    Write-Host "    next version: $UpdateVersion"
}

Write-Host "==> Creating zero-filled payload $payloadPath (1024 bytes)"
[System.IO.File]::WriteAllBytes($payloadPath, [byte[]]::new(1024))

if ($updateAlreadyImported) {
    Write-Host "==> Update $UpdateProvider/$UpdateName/$UpdateVersion already imported; reusing it"
} else {
    Write-Host "==> Generating v5 import manifest (version $UpdateVersion)"
    # az CLI parses --step as space-separated key=value tokens. The properties
    # value must be JSON with the inner double-quotes backslash-escaped so they
    # survive the az.cmd/Python argument parsing on Windows.
    $properties = "{\""installedCriteria\"":\""$UpdateVersion\""}"
    az iot du update init v5 `
        --update-provider $UpdateProvider --update-name $UpdateName --update-version $UpdateVersion `
        --compat manufacturer=Contoso model=ADU-Sim `
        --step handler=microsoft/swupdate:1 properties=$properties `
        --file path=$payloadPath > $manifestPath

    Write-Host "==> Staging + importing the update"
    az iot du update stage `
        --account $AccountName --instance $InstanceName `
        --storage-account $StorageAccount --storage-container $ContainerName `
        --manifest-path $manifestPath --then-import --overwrite
}

Write-Host "==> Tagging device $DeviceId into group $GroupId"
$tags = "{\""ADUGroup\"":\""$GroupId\""}"
az iot hub device-twin update --hub-name $IotHubName --device-id $DeviceId --tags $tags | Out-Null

# ADU does not read the hub live: it IMPORTS device twins into its own store on a
# periodic schedule and only then classifies tagged devices into groups. Relying
# on that schedule makes the group appear minutes later (or not within this
# script's wait), so trigger an on-demand import now. It is async; the group-wait
# loop below polls until the device has been classified.
Write-Host "==> Triggering ADU device import from the hub"
az iot du device import `
    --account $AccountName --instance $InstanceName --import-type Devices `
    --only-show-errors 2>$null | Out-Null

if (-not $updateAlreadyImported) {
    Write-Host "==> Waiting for imported update $UpdateProvider/$UpdateName/$UpdateVersion to become available"
    $updateReady = $false
    for ($i = 0; $i -lt 60; $i++) {
        az iot du update show `
            --account $AccountName --instance $InstanceName `
            --update-provider $UpdateProvider --update-name $UpdateName --update-version $UpdateVersion `
            --only-show-errors 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $updateReady = $true; break }
        Start-Sleep -Seconds 10
        Write-Host "    still importing... ($([int](($i + 1) * 10))s)"
    }
    if (-not $updateReady) {
        Write-Error "Update did not finish importing in time. Re-run this script in a few minutes."
        exit 1
    }
    Write-Host "    update is available."
}

Write-Host "==> Creating deployment $deploymentId"
# Two things must have propagated before a deployment can be created, and either
# can lag behind:
#   1. The device group only exists once the on-demand import (triggered above)
#      finishes and classifies the tagged device into it. The first import of a
#      device can take a few minutes.
#   2. A freshly-imported update takes a moment to become deployable even after
#      'update show' returns it.
# Both surface as a 404 from 'deployment create', so wait for the group, then
# retry the create a few times. Re-trigger the import periodically in case the
# first one had not registered the tag yet.
Write-Host "    waiting for device group $GroupId to be visible to ADU"
$groupReady = $false
for ($i = 0; $i -lt 60; $i++) {
    az iot du device group show `
        --account $AccountName --instance $InstanceName --group-id $GroupId `
        --only-show-errors 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { $groupReady = $true; break }
    # Nudge the import again every ~60s; harmless if one is already in flight.
    if ($i -gt 0 -and ($i % 6) -eq 0) {
        az iot du device import `
            --account $AccountName --instance $InstanceName --import-type Devices `
            --only-show-errors 2>$null | Out-Null
    }
    Start-Sleep -Seconds 10
    Write-Host "    group not visible yet... ($([int](($i + 1) * 10))s) — importing the tagged device into ADU"
}
if (-not $groupReady) {
    Write-Error "Device group '$GroupId' never appeared. Ensure the device sample is running and connected (so its twin carries the ADUGroup tag), then re-run this script."
    exit 1
}

$deployed = $false
for ($i = 0; $i -lt 12; $i++) {
    az iot du device deployment create `
        --account $AccountName --instance $InstanceName `
        --group-id $GroupId --deployment-id $deploymentId `
        --update-name $UpdateName --update-provider $UpdateProvider --update-version $UpdateVersion `
        --start-time $startTime --only-show-errors 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { $deployed = $true; break }
    Start-Sleep -Seconds 10
    Write-Host "    deployment not accepted yet (update still propagating)... ($([int](($i + 1) * 10))s)"
}
if (-not $deployed) {
    Write-Error "Deployment create kept failing. The update may still be propagating; re-run this script in a few minutes."
    exit 1
}

# Remember what we created so the cleanup script can target it.
$env:AZ_IOT_ADU_DEPLOYMENT     = $deploymentId
$env:AZ_IOT_ADU_UPDATE_VERSION = $UpdateVersion

Write-Host ""
Write-Host "Deployment created."
Write-Host "  Update     : $UpdateProvider/$UpdateName/$UpdateVersion"
Write-Host "  Group      : $GroupId"
Write-Host "  Deployment : $deploymentId"
Write-Host ""
Write-Host "Watch the running device sample pick it up. Track service status with:"
Write-Host "  az iot du device deployment show --account $AccountName --instance $InstanceName --group-id $GroupId --deployment-id $deploymentId --status -o json"
