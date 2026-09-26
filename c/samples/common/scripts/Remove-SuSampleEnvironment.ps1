#Requires -Version 7.0
<#
.SYNOPSIS
    Tears down the resources created by Initialize-SuSampleEnvironment.ps1, or
    just the last deployment/update.

.DESCRIPTION
    -DeploymentOnly removes a deployment and an imported update through
    'az iot du ...', i.e. the IoT-Hub-based Device Update model. The software updates samples
    in this repository implement the DPS-fronted model, whose deployments are
    Azure Device Registry jobs and runs; this script does not touch those.
    See samples/su/pc/README.md.

    By default deletes the entire resource group created by
    Initialize-SuSampleEnvironment.ps1 (the simplest, complete cleanup).

    Use -DeploymentOnly to remove just the deployment and imported update while
    leaving the hub/DPS/software updates/storage in place, so you can run
    New-SuSampleDeployment.ps1 again without re-provisioning.

.EXAMPLE
    ./Remove-SuSampleEnvironment.ps1

.EXAMPLE
    ./Remove-SuSampleEnvironment.ps1 -DeploymentOnly
#>
param(
    [switch]$DeploymentOnly,
    [string]$ResourceGroup  = $env:AZ_IOT_SU_RESOURCE_GROUP,
    [string]$AccountName    = $env:AZ_IOT_SU_ACCOUNT,
    [string]$InstanceName   = $env:AZ_IOT_SU_INSTANCE,
    [string]$GroupId        = $env:AZ_IOT_SU_GROUP,
    [string]$DeploymentId   = $env:AZ_IOT_SU_DEPLOYMENT,
    [string]$IotHubName     = $env:AZ_IOT_SU_IOTHUB,
    [string]$DiagSettingName = $(if ($env:AZ_IOT_SU_DIAG_SETTING) { $env:AZ_IOT_SU_DIAG_SETTING } else { "su-sim-conn-diag" }),
    [string]$UpdateProvider = "Contoso",
    [string]$UpdateName     = "SU-Sim",
    [string]$UpdateVersion  = $env:AZ_IOT_SU_UPDATE_VERSION
)

Write-Warning @'
The -DeploymentOnly mode deletes a deployment and an imported update through
'az iot du ...', i.e. the IoT-Hub-based Device Update model. The software updates samples in
this repository implement the DPS-fronted model, whose deployments are Azure
Device Registry jobs and runs; this script does not remove those. Deleting the
resource group removes everything it contains either way.

See samples/su/pc/README.md.
'@

if ($DeploymentOnly) {
    Write-Host "==> Deleting deployment $DeploymentId"
    az iot du device deployment delete --account $AccountName --instance $InstanceName --group-id $GroupId --deployment-id $DeploymentId -y

    Write-Host "==> Deleting imported update $UpdateProvider/$UpdateName/$UpdateVersion"
    az iot du update delete --account $AccountName --instance $InstanceName --update-provider $UpdateProvider --update-name $UpdateName --update-version $UpdateVersion -y

    Write-Host ""
    Write-Host "Deployment and update removed. Infrastructure left in place; re-run ./New-SuSampleDeployment.ps1 to deploy again."
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
    Write-Host "Resource group deletion started. Local cert/payload/manifest files (su-sim-*) can be removed manually."
}
