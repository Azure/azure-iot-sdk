# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

#Requires -Version 7.0

<#
.SYNOPSIS
    Service side of the software updates e2e suite: stages the offers the
    device suite consumes, checks what the service recorded, and cleans up.

.DESCRIPTION
    One offer per scenario. Each is an imported update whose compatibility is
    unique to the run and the scenario, plus an Azure Device Registry
    OnboardingUpdate job for it, so runs and scenarios never see each other's
    offers.

    Calls the published REST APIs (api-version 2026-11-02-preview):
      - Device Update data plane: updates:import, updates/operations, updates DELETE
      - Microsoft.DeviceRegistry: namespaces/jobs, jobs/runs, runs/listResults
    Tokens and blob uploads go through the Azure CLI, which must be logged in.

    Configuration, all required:
      AZ_IOT_E2E_SU_SUBSCRIPTION_ID   subscription of the environment
      AZ_IOT_E2E_SU_RESOURCE_GROUP    its resource group
      AZ_IOT_E2E_SU_LOCATION          location of the ADR namespace
      AZ_IOT_E2E_SU_ADR_NAMESPACE     ADR namespace linked to the update instance
      AZ_IOT_E2E_SU_ADU_ENDPOINT      Device Update data-plane host name
      AZ_IOT_E2E_SU_STORAGE_ACCOUNT   storage the update files are imported from
      AZ_IOT_E2E_SU_STORAGE_CONTAINER container in it
    Optional:
      AZ_IOT_E2E_SU_ARM_ENDPOINT      ARM endpoint for requests (may be regional);
                                      default https://management.azure.com. Tokens
                                      are always for https://management.azure.com/.

.EXAMPLE
    Import-Module ./SuE2E.psm1
    New-SuE2EOffers -RunId $env:GITHUB_RUN_ID -StatePath ./su-offers.json
    # ... run ctest -R e2e_su ...
    Test-SuE2EOffers -StatePath ./su-offers.json -RegistrationId $env:AZ_IOT_E2E_SU_REG_ID
    Remove-SuE2EOffers -StatePath ./su-offers.json
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ApiVersion = '2026-11-02-preview'
$script:AduScope = 'https://api.adu.microsoft.com/'
# Token audience for Azure Resource Manager. Fixed: a regional ARM endpoint
# (AZ_IOT_E2E_SU_ARM_ENDPOINT) serves requests but is not a token audience.
$script:ArmScope = 'https://management.azure.com/'

# Scenario name -> device-suite variable holding its model, and the per-device
# job result status the service is expected to record once the suite has run.
$script:Scenarios = [ordered]@{
    REAL              = @{ Short = 'real'; Variable = 'AZ_IOT_E2E_SU_OFFER_MODEL'; Expected = 'Succeeded' }
    INSTALL_FAILURE   = @{ Short = 'fail'; Variable = 'AZ_IOT_E2E_SU_OFFER_MODEL_INSTALL_FAILURE'; Expected = 'Failed' }
    # E2E-PLACEHOLDER: status recorded for a SKIPPED report is unmeasured.
    ALREADY_INSTALLED = @{ Short = 'skip'; Variable = 'AZ_IOT_E2E_SU_OFFER_MODEL_ALREADY_INSTALLED'; Expected = 'Succeeded' }
    UNTRUSTED         = @{ Short = 'untrusted'; Variable = 'AZ_IOT_E2E_SU_OFFER_MODEL_UNTRUSTED'; Expected = 'Failed' }
}

function Get-SuE2EConfig {
    $names = 'SUBSCRIPTION_ID', 'RESOURCE_GROUP', 'LOCATION', 'ADR_NAMESPACE', 'ADU_ENDPOINT',
    'STORAGE_ACCOUNT', 'STORAGE_CONTAINER'
    $config = @{}
    $missing = @()
    foreach ($n in $names) {
        $v = [Environment]::GetEnvironmentVariable("AZ_IOT_E2E_SU_$n")
        if ([string]::IsNullOrEmpty($v)) { $missing += "AZ_IOT_E2E_SU_$n" } else { $config[$n] = $v }
    }
    if ($missing) { throw "Missing configuration: $($missing -join ', ')" }
    $arm = $env:AZ_IOT_E2E_SU_ARM_ENDPOINT
    $config.ARM_ENDPOINT = if ($arm) { $arm.TrimEnd('/') } else { 'https://management.azure.com' }
    return $config
}

function Get-SuE2EToken([string]$Resource) {
    $token = az account get-access-token --resource $Resource --query accessToken -o tsv
    if ($LASTEXITCODE -ne 0 -or -not $token) { throw "Could not get a token for $Resource" }
    return $token
}

function Invoke-SuE2ERest {
    param(
        [Parameter(Mandatory)][string]$Method,
        [Parameter(Mandatory)][string]$Uri,
        [Parameter(Mandatory)][string]$Token,
        [object]$Body
    )
    $params = @{
        Method                  = $Method
        Uri                     = $Uri
        Headers                 = @{ Authorization = "Bearer $Token" }
        ContentType             = 'application/json'
        ResponseHeadersVariable = 'headers'
        StatusCodeVariable      = 'status'
    }
    if ($null -ne $Body) { $params.Body = ($Body | ConvertTo-Json -Depth 20) }
    $response = Invoke-RestMethod @params
    return [pscustomobject]@{ Status = $status; Headers = $headers; Body = $response }
}

<#
.SYNOPSIS
    Waits for the long-running operation a response started, if it started
    one; throws unless it ends in success.
#>
function Wait-SuE2EOperation {
    param(
        [Parameter(Mandatory)][object]$Response,
        [Parameter(Mandatory)][string]$Token,
        [Parameter(Mandatory)][string]$BaseUri,
        [int]$Minutes = 5
    )
    if ($Response.Status -ne 202) { return }
    $h = $Response.Headers
    $status = $null
    foreach ($name in 'Azure-AsyncOperation', 'Operation-Location') {
        if ($h -and $h.ContainsKey($name)) { $status = @($h[$name])[0]; break }
    }
    $location = if ($h -and $h.ContainsKey('Location')) { @($h['Location'])[0] } else { $null }
    $uri = if ($status) { $status } else { $location }
    if (-not $uri) { return }
    if ($uri -notmatch '^https?://') { $uri = "$BaseUri$uri" }

    $deadline = (Get-Date).AddMinutes($Minutes)
    while ($true) {
        $r = Invoke-SuE2ERest -Method Get -Uri $uri -Token $Token
        if ($status) {
            # Status monitor: the body carries the state.
            $state = $r.Body.status
            if ($state -eq 'Succeeded') { return }
            if ($state -in 'Failed', 'Canceled') { throw "Operation $state`: $($r.Body | ConvertTo-Json -Depth 10 -Compress)" }
        }
        elseif ($r.Status -ne 202) {
            return # Location: anything but 202 is the final answer.
        }
        if ((Get-Date) -gt $deadline) { throw "Operation did not finish in $Minutes min: $uri" }
        Start-Sleep -Seconds 5
    }
}

function Get-SuE2EJobsUri([hashtable]$Config) {
    return "$($Config.ARM_ENDPOINT)/subscriptions/$($Config.SUBSCRIPTION_ID)/resourceGroups/" +
    "$($Config.RESOURCE_GROUP)/providers/Microsoft.DeviceRegistry/namespaces/$($Config.ADR_NAMESPACE)/jobs"
}

<#
.SYNOPSIS
    Imports a one-file, one-step update compatible with exactly one
    manufacturer/model pair, and waits for the import to finish.
#>
function Import-SuE2EUpdate {
    param(
        [Parameter(Mandatory)][hashtable]$Config,
        [Parameter(Mandatory)][string]$Provider,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$Manufacturer,
        [Parameter(Mandatory)][string]$Model,
        [Parameter(Mandatory)][string]$WorkDir,
        # Receives each uploaded blob name as soon as it exists, for cleanup.
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Blobs,
        # Called after each upload, so the caller can persist $Blobs at once.
        [scriptblock]$OnChange = {}
    )
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

    # Random content, so the device's hash check is meaningful.
    $payload = Join-Path $WorkDir "$Model.bin"
    $bytes = [byte[]]::new(4096)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    [System.IO.File]::WriteAllBytes($payload, $bytes)
    $payloadHash = [Convert]::ToBase64String([System.Security.Cryptography.SHA256]::HashData($bytes))

    $manifest = [ordered]@{
        updateId        = [ordered]@{ provider = $Provider; name = $Name; version = $Version }
        compatibility   = @([ordered]@{ manufacturer = $Manufacturer; model = $Model })
        instructions    = @{
            steps = @([ordered]@{
                    handler           = 'microsoft/swupdate:1'
                    files             = @("$Model.bin")
                    handlerProperties = @{ installedCriteria = $Version }
                })
        }
        files           = @([ordered]@{
                filename    = "$Model.bin"
                sizeInBytes = $bytes.Length
                hashes      = @{ sha256 = $payloadHash }
            })
        createdDateTime = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        manifestVersion = '5.0'
    }
    $manifestPath = Join-Path $WorkDir "$Model.importmanifest.json"
    $manifestText = $manifest | ConvertTo-Json -Depth 20
    [System.IO.File]::WriteAllText($manifestPath, $manifestText)
    $manifestBytes = [System.IO.File]::ReadAllBytes($manifestPath)

    $urls = @{}
    foreach ($file in $payload, $manifestPath) {
        $blob = "$Provider/$Name/$Version/$(Split-Path -Leaf $file)"
        az storage blob upload --auth-mode login --account-name $Config.STORAGE_ACCOUNT `
            --container-name $Config.STORAGE_CONTAINER --name $blob --file $file --overwrite --only-show-errors | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Upload of $blob failed" }
        $Blobs.Add($blob)
        & $OnChange
        $expiry = (Get-Date).ToUniversalTime().AddHours(6).ToString('yyyy-MM-ddTHH:mmZ')
        $url = az storage blob generate-sas --auth-mode login --as-user --account-name $Config.STORAGE_ACCOUNT `
            --container-name $Config.STORAGE_CONTAINER --name $blob --permissions r --expiry $expiry --full-uri -o tsv
        if ($LASTEXITCODE -ne 0 -or -not $url) { throw "SAS for $blob failed" }
        $urls[$file] = $url
    }

    $body = @{
        importUpdateInput = @(@{
                importManifest = @{
                    url         = $urls[$manifestPath]
                    sizeInBytes = $manifestBytes.Length
                    hashes      = @{ sha256 = [Convert]::ToBase64String([System.Security.Cryptography.SHA256]::HashData($manifestBytes)) }
                }
                files          = @(@{ fileName = "$Model.bin"; url = $urls[$payload] })
            })
    }
    $token = Get-SuE2EToken $script:AduScope
    $base = "https://$($Config.ADU_ENDPOINT)"
    $r = Invoke-SuE2ERest -Method Post -Uri "$base/updates:import?api-version=$script:ApiVersion" -Token $token -Body $body
    $operation = @($r.Headers['Operation-Location'])[0]
    if (-not $operation) { throw 'Import returned no Operation-Location' }
    if ($operation -notmatch '^https?://') { $operation = "$base$operation" }

    $deadline = (Get-Date).AddMinutes(10)
    while ($true) {
        $op = (Invoke-SuE2ERest -Method Get -Uri $operation -Token $token).Body
        if ($op.status -eq 'Succeeded') { break }
        if ($op.status -eq 'Failed') { throw "Import of $Provider/$Name/$Version failed: $($op | ConvertTo-Json -Depth 10 -Compress)" }
        if ((Get-Date) -gt $deadline) { throw "Import of $Provider/$Name/$Version did not finish in time" }
        Start-Sleep -Seconds 10
    }
    return "updates/providers/$Provider/names/$Name/versions/$Version"
}

function New-SuE2EOnboardingJob {
    param(
        [Parameter(Mandatory)][hashtable]$Config,
        [Parameter(Mandatory)][string]$JobName,
        [Parameter(Mandatory)][string]$UpdateResourceId
    )
    $token = Get-SuE2EToken $script:ArmScope
    $uri = "$(Get-SuE2EJobsUri $Config)/$($JobName)?api-version=$script:ApiVersion"
    $body = @{
        location   = $Config.LOCATION
        properties = @{
            jobType     = 'OnboardingUpdate'
            description = 'Software updates e2e offer.'
            definition  = @{ schedulingType = 'Continuous'; updateResourceId = $UpdateResourceId }
        }
    }
    Invoke-SuE2ERest -Method Put -Uri $uri -Token $token -Body $body | Out-Null

    $deadline = (Get-Date).AddMinutes(5)
    while ($true) {
        $job = (Invoke-SuE2ERest -Method Get -Uri $uri -Token $token).Body
        $state = $job.properties.provisioningState
        if ($state -eq 'Succeeded') { return }
        if ($state -in 'Failed', 'Canceled') { throw "Job $JobName provisioning $state" }
        if ((Get-Date) -gt $deadline) { throw "Job $JobName did not provision in time" }
        Start-Sleep -Seconds 10
    }
}

<#
.SYNOPSIS
    Returns the result entry the newest run of a job holds for one onboarding
    device, or $null when it holds none.
#>
function Get-SuE2EDeviceResult {
    param(
        [Parameter(Mandatory)][hashtable]$Config,
        [Parameter(Mandatory)][string]$JobName,
        [Parameter(Mandatory)][string]$RegistrationId
    )
    $token = Get-SuE2EToken $script:ArmScope
    $job = "$(Get-SuE2EJobsUri $Config)/$JobName"
    # E2E-PLACEHOLDER: assumes a Continuous onboarding job carries its runs in
    # runs/, newest by startTime; unmeasured.
    $runs = @((Invoke-SuE2ERest -Method Get -Uri "$job/runs?api-version=$script:ApiVersion" -Token $token).Body.value)
    if (-not $runs) { return $null }
    # StrictMode: entries may omit optional properties, so each is probed before use.
    $run = $runs | Sort-Object { $p = $_.properties; if ($p.PSObject.Properties['startTime']) { $p.startTime } else { '' } } -Descending |
        Select-Object -First 1
    $uri = "$job/runs/$($run.name)/listResults?api-version=$script:ApiVersion"
    $body = @{}
    while ($true) {
        $page = (Invoke-SuE2ERest -Method Post -Uri $uri -Token $token -Body $body).Body
        $hit = @($page.value) | Where-Object {
            $_.PSObject.Properties['resourceExternalId'] -and $_.resourceExternalId -eq $RegistrationId
        } | Select-Object -First 1
        if ($hit) { return $hit }
        # Paged by a skipToken sent back in the next request body.
        $next = if ($page.PSObject.Properties['skipToken']) { $page.skipToken } else { $null }
        if (-not $next) { return $null }
        $body = @{ skipToken = $next }
    }
}

<#
.SYNOPSIS
    Stages one offer per scenario, records them in -StatePath, and exports the
    variables the device suite reads (also to $env:GITHUB_ENV when set).
#>
function New-SuE2EOffers {
    param(
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$StatePath,
        [string]$WorkDir = (Join-Path ([System.IO.Path]::GetTempPath()) "su-e2e-$RunId")
    )
    $config = Get-SuE2EConfig
    $tag = ($RunId -replace '[^A-Za-z0-9]', '').ToLowerInvariant()
    $manufacturer = "e2e$tag"
    $state = [ordered]@{ Manufacturer = $manufacturer; Offers = [ordered]@{} }
    $exports = [ordered]@{ AZ_IOT_E2E_SU_OFFER_MANUFACTURER = $manufacturer }

    # Saved before every remote call, so a run killed mid-step still leaves
    # Remove-SuE2EOffers everything it may have created.
    $save = { $state | ConvertTo-Json -Depth 5 | Set-Content -Path $StatePath }

    foreach ($scenario in $script:Scenarios.Keys) {
        # Short names: the whole offer must fit the client's 4 KiB request buffer.
        $short = $script:Scenarios[$scenario].Short
        $model = "e2e-$tag-$short"
        $offer = [ordered]@{
            Model  = $model
            Blobs  = [System.Collections.Generic.List[string]]::new()
            Update = "updates/providers/sdke2e/names/$model/versions/1.0.0"
            Job    = $null
        }
        $state.Offers[$scenario] = $offer
        & $save
        $update = Import-SuE2EUpdate -Config $config -Provider 'sdke2e' -Name $model `
            -Version '1.0.0' -Manufacturer $manufacturer -Model $model -WorkDir $WorkDir `
            -Blobs $offer.Blobs -OnChange $save
        $offer.Job = "su-e2e-$tag-$short"
        & $save
        New-SuE2EOnboardingJob -Config $config -JobName $offer.Job -UpdateResourceId $update
        $exports[$script:Scenarios[$scenario].Variable] = $model
    }

    foreach ($k in $exports.Keys) {
        Set-Item -Path "env:$k" -Value $exports[$k]
        if ($env:GITHUB_ENV) { Add-Content -Path $env:GITHUB_ENV -Value "$k=$($exports[$k])" }
    }
}

<#
.SYNOPSIS
    Fails unless the service recorded the expected per-device status for every
    staged offer.
#>
function Test-SuE2EOffers {
    param(
        [Parameter(Mandatory)][string]$StatePath,
        [Parameter(Mandatory)][string]$RegistrationId,
        # Results are projected asynchronously, so absent or InProgress is waited out.
        [int]$Minutes = 10
    )
    $config = Get-SuE2EConfig
    $state = Get-Content -Raw -Path $StatePath | ConvertFrom-Json -AsHashtable
    $deadline = (Get-Date).AddMinutes($Minutes)
    $failures = @()
    foreach ($scenario in $script:Scenarios.Keys) {
        $offer = $state.Offers[$scenario]
        $expected = $script:Scenarios[$scenario].Expected
        while ($true) {
            $result = if ($offer -and $offer.Job) { Get-SuE2EDeviceResult -Config $config -JobName $offer.Job -RegistrationId $RegistrationId }
            $actual = if ($result -and $result.PSObject.Properties['status']) { $result.status } else { '<none>' }
            if ($actual -notin '<none>', 'InProgress' -or (Get-Date) -gt $deadline) { break }
            Start-Sleep -Seconds 15
        }
        Write-Host "$scenario`: job result $actual (expected $expected)"
        if ($actual -ne $expected) { $failures += $scenario }
    }
    if ($failures) { throw "Unexpected job results: $($failures -join ', ')" }
}

<#
.SYNOPSIS
    Deletes the jobs and updates recorded in -StatePath. Best effort: every
    item is attempted, and the first error is rethrown at the end.
#>
function Remove-SuE2EOffers {
    param([Parameter(Mandatory)][string]$StatePath)
    if (-not (Test-Path $StatePath)) { return }
    $config = Get-SuE2EConfig
    $state = Get-Content -Raw -Path $StatePath | ConvertFrom-Json -AsHashtable
    $armToken = Get-SuE2EToken $script:ArmScope
    $aduToken = Get-SuE2EToken $script:AduScope
    $firstError = $null
    foreach ($offer in $state.Offers.Values) {
        # Each item separately, so one failure does not strand the others.
        $items = @()
        if ($offer.Job) { $items += , @('Job', $offer.Job) }
        if ($offer.Update) { $items += , @('Update', $offer.Update) }
        foreach ($blob in @($offer.Blobs)) { $items += , @('Blob', $blob) }
        foreach ($item in $items) {
            $kind, $name = $item
            try {
                switch ($kind) {
                    'Job' {
                        # Awaited: the update it references is deleted next.
                        $r = Invoke-SuE2ERest -Method Delete -Token $armToken `
                            -Uri "$(Get-SuE2EJobsUri $config)/$($name)?api-version=$script:ApiVersion"
                        Wait-SuE2EOperation -Response $r -Token $armToken -BaseUri $config.ARM_ENDPOINT
                    }
                    'Update' {
                        $r = Invoke-SuE2ERest -Method Delete -Token $aduToken `
                            -Uri "https://$($config.ADU_ENDPOINT)/$($name)?api-version=$script:ApiVersion"
                        Wait-SuE2EOperation -Response $r -Token $aduToken -BaseUri "https://$($config.ADU_ENDPOINT)"
                    }
                    'Blob' {
                        az storage blob delete --auth-mode login --account-name $config.STORAGE_ACCOUNT `
                            --container-name $config.STORAGE_CONTAINER --name $name --only-show-errors | Out-Null
                        if ($LASTEXITCODE -ne 0) { throw "Delete of blob $name failed" }
                    }
                }
            }
            catch {
                # Recorded before it was created, so it may never have existed.
                $code = $_.Exception.PSObject.Properties['Response'] ? $_.Exception.Response.StatusCode : $null
                if ($code -eq [System.Net.HttpStatusCode]::NotFound) { continue }
                Write-Warning "Cleanup of $kind $name failed: $_"
                if (-not $firstError) { $firstError = $_ }
            }
        }
    }
    if ($firstError) { throw $firstError }
}

Export-ModuleMember -Function New-SuE2EOffers, Test-SuE2EOffers, Remove-SuE2EOffers, Import-SuE2EUpdate,
New-SuE2EOnboardingJob, Get-SuE2EDeviceResult
