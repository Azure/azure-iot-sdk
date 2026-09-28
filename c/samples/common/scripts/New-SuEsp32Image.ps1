#Requires -Version 7.0
<#
.SYNOPSIS
    Builds an ESP32 firmware update image and deploys it through the
    IoT-Hub-based Device Update model.

.DESCRIPTION
    IMPORTANT - which Device Update model this targets.

    The import and deployment steps use 'az iot du ...', which is scoped to a
    Device Update account + instance: the IoT-Hub-based model. The software updates samples in
    this repository implement the DPS-fronted model, which has no accounts and
    delivers updates as Azure Device Registry jobs and runs. An update deployed
    by this script will not be offered to those samples. The image build steps
    are unaffected. See samples/software_update/pc/simulated_onboarding/README.md.

    The ESP32 counterpart of New-SuSampleDeployment.ps1. Where that script ships
    a zero-filled simulated payload, this one BUILDS a genuine ESP32 app image
    (the sample firmware) with the new version baked in, then imports + deploys
    it. An IoT-Hub-based Device Update consumer downloads and flashes that image
    over the air; the ESP32 sample in this repository is not one, so it is never
    offered the deployment.

    Run this AFTER Initialize-SuSampleEnvironment.ps1 (which creates the Azure
    resources + device cert and sets the AZ_IOT_SU_* environment variables).
    The deployment steps additionally need an IoT-Hub-based consumer already
    connected to the hub, so its twin can be tagged.

    Steps:
      1. Resolve the next update version (auto-bump, or -UpdateVersion).
      2. Rewrite main/su_version.h so the firmware reports that version.
      3. Source the ESP-IDF environment and `idf.py build` the sample.
      4. Generate a v5 import manifest referencing the built .bin.
      5. Stage + import the update, tag the device, create the deployment.

.PARAMETER IdfExportScript
    Path to the ESP-IDF export script. The PowerShell variant (export.ps1) next
    to it is dot-sourced to bring idf.py onto PATH. Defaults to the path the user
    provided: C:\esp\v6.0\esp-idf\export.bat (export.ps1 is derived from it).

.EXAMPLE
    ./New-SuEsp32Image.ps1

.EXAMPLE
    ./New-SuEsp32Image.ps1 -UpdateVersion 2.0.0
#>
param(
    [string]$ResourceGroup  = $env:AZ_IOT_SU_RESOURCE_GROUP,
    [string]$IotHubName     = $env:AZ_IOT_SU_IOTHUB,
    [string]$AccountName    = $env:AZ_IOT_SU_ACCOUNT,
    [string]$InstanceName   = $env:AZ_IOT_SU_INSTANCE,
    [string]$StorageAccount = $env:AZ_IOT_SU_STORAGE,
    [string]$ContainerName  = $env:AZ_IOT_SU_CONTAINER,
    [string]$DeviceId       = $env:AZ_IOT_SU_DEVICE_ID,
    [string]$GroupId        = $env:AZ_IOT_SU_GROUP,
    [string]$UpdateProvider = "Contoso",
    [string]$UpdateName     = "ESP32-SU",
    [string]$UpdateVersion  = "",
    [string]$Manufacturer   = "Espressif",
    [string]$Model          = "ESP32-WROOM",
    [string]$IdfExportScript = "C:\esp\v6.0\esp-idf\export.bat",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

Write-Warning @'
The import and deployment steps of this script target the IoT-Hub-based Device
Update model (account + instance). The software updates samples in this repository implement
the DPS-fronted model, which delivers updates as Azure Device Registry jobs and
runs. An update deployed by this script will not be offered to those samples.
The firmware image build is unaffected.

See samples/software_update/pc/simulated_onboarding/README.md.
'@

# Locate the ESP32 project (../../software_update/esp32 relative to this script).
$projectDir   = (Resolve-Path (Join-Path $PSScriptRoot "..\..\software_update\esp32")).Path
$versionHdr   = Join-Path $projectDir "main\su_version.h"
$firmwareBin  = Join-Path $projectDir "build\su_esp32.bin"
$manifestPath = Join-Path $PWD "su-esp32-manifest.importmanifest.json"
$deploymentId = "su-esp32-deploy-$(Get-Date -Format yyyyMMddHHmmss)"
$startTime    = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

# --- 1. Resolve the next version (immutable updates => never reuse) ----------
if ($UpdateVersion) {
    Write-Host "==> Using requested version $UpdateVersion"
} else {
    Write-Host "==> Determining the next update version for $UpdateProvider/$UpdateName"
    $existing = az iot du update list `
        --account $AccountName --instance $InstanceName `
        --update-provider $UpdateProvider --update-name $UpdateName `
        --only-show-errors 2>$null | ConvertFrom-Json

    $versions = @()
    foreach ($item in @($existing)) {
        $v = if ($item -is [string]) { $item }
             elseif ($item.version) { $item.version }
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

# --- 2. Bake the version into the firmware -----------------------------------
Write-Host "==> Writing $versionHdr (version $UpdateVersion)"
$header = @"
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Generated by New-SuEsp32Image.ps1 — do not edit by hand. */
#ifndef SU_VERSION_H
#define SU_VERSION_H

#define SU_UPDATE_PROVIDER "$UpdateProvider"
#define SU_UPDATE_NAME     "$UpdateName"
#define SU_UPDATE_VERSION  "$UpdateVersion"

#endif /* SU_VERSION_H */
"@
[System.IO.File]::WriteAllText($versionHdr, ($header -replace "`r`n", "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

# --- 3. Build the firmware with ESP-IDF --------------------------------------
if (-not $SkipBuild) {
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        # Bring the ESP-IDF tools onto PATH for this session via export.ps1.
        $exportPs1 = [System.IO.Path]::ChangeExtension($IdfExportScript, ".ps1")
        if (-not (Test-Path $exportPs1)) {
            throw "ESP-IDF export script not found: $exportPs1 (derived from -IdfExportScript $IdfExportScript). Open an ESP-IDF PowerShell or pass -IdfExportScript."
        }
        Write-Host "==> Sourcing ESP-IDF environment: $exportPs1"
        . $exportPs1
    }
    Write-Host "==> Building ESP32 firmware (idf.py build)"
    idf.py -C $projectDir build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed." }
}

if (-not (Test-Path $firmwareBin)) {
    throw "Firmware image not found: $firmwareBin. Build the project first (or omit -SkipBuild)."
}
$binSize = (Get-Item $firmwareBin).Length
Write-Host "==> Firmware image: $firmwareBin ($binSize bytes)"

# --- 4. Generate the v5 import manifest --------------------------------------
Write-Host "==> Generating v5 import manifest (version $UpdateVersion)"
$properties = "{\""installedCriteria\"":\""$UpdateVersion\""}"
az iot du update init v5 `
    --update-provider $UpdateProvider --update-name $UpdateName --update-version $UpdateVersion `
    --compat manufacturer=$Manufacturer model=$Model `
    --step handler=microsoft/swupdate:1 properties=$properties `
    --file path=$firmwareBin > $manifestPath

# --- 5. Stage + import, tag the device, deploy -------------------------------
Write-Host "==> Staging + importing the update"
az iot du update stage `
    --account $AccountName --instance $InstanceName `
    --storage-account $StorageAccount --storage-container $ContainerName `
    --manifest-path $manifestPath --then-import --overwrite

Write-Host "==> Tagging device $DeviceId into group $GroupId"
$tags = "{\""ADUGroup\"":\""$GroupId\""}"
az iot hub device-twin update --hub-name $IotHubName --device-id $DeviceId --tags $tags | Out-Null

Write-Host "==> Triggering software updates device import from the hub"
az iot du device import `
    --account $AccountName --instance $InstanceName --import-type Devices `
    --only-show-errors 2>$null | Out-Null

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
    Write-Error "Update did not finish importing in time. Re-run this script (with -SkipBuild) in a few minutes."
    exit 1
}

Write-Host "==> Waiting for device group $GroupId to be visible to software updates"
$groupReady = $false
for ($i = 0; $i -lt 60; $i++) {
    az iot du device group show `
        --account $AccountName --instance $InstanceName --group-id $GroupId `
        --only-show-errors 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { $groupReady = $true; break }
    if ($i -gt 0 -and ($i % 6) -eq 0) {
        az iot du device import `
            --account $AccountName --instance $InstanceName --import-type Devices `
            --only-show-errors 2>$null | Out-Null
    }
    Start-Sleep -Seconds 10
    Write-Host "    group not visible yet... ($([int](($i + 1) * 10))s)"
}
if (-not $groupReady) {
    Write-Error "Device group '$GroupId' never appeared. Ensure the ESP32 sample is running and connected, then re-run with -SkipBuild."
    exit 1
}

Write-Host "==> Creating deployment $deploymentId"
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
    Write-Error "Deployment create kept failing; re-run with -SkipBuild in a few minutes."
    exit 1
}

$env:AZ_IOT_SU_DEPLOYMENT     = $deploymentId
$env:AZ_IOT_SU_UPDATE_VERSION = $UpdateVersion

Write-Host ""
Write-Host "Deployment created."
Write-Host "  Update     : $UpdateProvider/$UpdateName/$UpdateVersion"
Write-Host "  Image      : $firmwareBin ($binSize bytes)"
Write-Host "  Group      : $GroupId"
Write-Host "  Deployment : $deploymentId"
Write-Host ""
Write-Host "Watch the ESP32 serial monitor (idf.py -C `"$projectDir`" monitor): it downloads"
Write-Host "the image, flashes the inactive OTA slot, reboots, and reports version $UpdateVersion."
Write-Host "Track service status with:"
Write-Host "  az iot du device deployment show --account $AccountName --instance $InstanceName --group-id $GroupId --deployment-id $deploymentId --status -o json"
