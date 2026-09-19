#Requires -Version 7.0
<#
.SYNOPSIS
    Tears down the resources created by Initialize-AduSampleEnvironment.ps1, or
    just the last deployment/update.

.DESCRIPTION
    -DeploymentOnly removes a deployment and an imported update through
    'az iot du ...', i.e. the IoT-Hub-based Device Update model. The ADU samples
    in this repository implement the DPS-fronted model, whose deployments are
    Azure Device Registry jobs and runs; this script does not touch those.
    See samples/adu/pc/README.md.

    By default deletes the entire resource group created by
    Initialize-AduSampleEnvironment.ps1 (the simplest, complete cleanup).

    Use -DeploymentOnly to remove just the deployment and imported update while
    leaving the hub/DPS/ADU/storage in place, so you can run
    New-AduSampleDeployment.ps1 again without re-provisioning.

.EXAMPLE
    ./Remove-AduSampleEnvironment.ps1

.EXAMPLE
    ./Remove-AduSampleEnvironment.ps1 -DeploymentOnly
#>
param(
    [switch]$DeploymentOnly,
    [string]$ResourceGroup  = $env:AZ_IOT_ADU_RESOURCE_GROUP,
    [string]$AccountName    = $env:AZ_IOT_ADU_ACCOUNT,
    [string]$InstanceName   = $env:AZ_IOT_ADU_INSTANCE,
    [string]$GroupId        = $env:AZ_IOT_ADU_GROUP,
    [string]$DeploymentId   = $env:AZ_IOT_ADU_DEPLOYMENT,
    [string]$IotHubName     = $env:AZ_IOT_ADU_IOTHUB,
    [string]$DiagSettingName = $(if ($env:AZ_IOT_ADU_DIAG_SETTING) { $env:AZ_IOT_ADU_DIAG_SETTING } else { "adu-sim-conn-diag" }),
    [string]$UpdateProvider = "Contoso",
    [string]$UpdateName     = "ADU-Sim",
    [string]$UpdateVersion  = $env:AZ_IOT_ADU_UPDATE_VERSION
)

if ($DeploymentOnly) {
    Write-Host "==> Deleting deployment $DeploymentId"
    az iot du device deployment delete --account $AccountName --instance $InstanceName --group-id $GroupId --deployment-id $DeploymentId -y

    Write-Host "==> Deleting imported update $UpdateProvider/$UpdateName/$UpdateVersion"
    az iot du update delete --account $AccountName --instance $InstanceName --update-provider $UpdateProvider --update-name $UpdateName --update-version $UpdateVersion -y

    Write-Host ""
    Write-Host "Deployment and update removed. Infrastructure left in place; re-run ./New-AduSampleDeployment.ps1 to deploy again."
}
else {
    if ($IotHubName -and $DiagSettingName) {
        Write-Host "==> Removing IoT Hub connection diagnostic setting $DiagSettingName"
        $hubId = az iot hub show --name $IotHubName --query id -o tsv 2>$null
        if ($hubId) {
            az monitor diagnostic-settings delete --name $DiagSettingName --resource $hubId 2>$null
        }
    }

    Write-Host "==> Deleting resource group $ResourceGroup (everything)"
    az group delete --name $ResourceGroup --yes --no-wait

    Write-Host ""
    Write-Host "Resource group deletion started. Local cert/payload/manifest files (adu-sim-*) can be removed manually."
}
