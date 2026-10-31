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
    OnboardingUpdate job for it, scheduled with one run, so runs and scenarios
    never see each other's
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
    New-SuE2EOffers -RunId $env:GITHUB_RUN_ID -StatePath ./su-offers.json   # -Reuse keeps what exists
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
    # A SKIPPED report is recorded as Succeeded (measured).
    ALREADY_INSTALLED = @{ Short = 'skip'; Variable = 'AZ_IOT_E2E_SU_OFFER_MODEL_ALREADY_INSTALLED'; Expected = 'Succeeded' }
    UNTRUSTED         = @{ Short = 'untrusted'; Variable = 'AZ_IOT_E2E_SU_OFFER_MODEL_UNTRUSTED'; Expected = 'Failed' }
    LIBRARY_MODE      = @{ Short = 'lib'; Variable = 'AZ_IOT_E2E_SU_OFFER_MODEL_LIBRARY_MODE'; Expected = 'Succeeded' }
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

# HTTP statuses retried as transient: timeout, throttling, server or gateway failure.
$script:TransientStatusCodes = 408, 429, 500, 502, 503, 504
# ARM and data-plane error codes (error.code) retried as transient on any other 5xx.
$script:TransientErrorCodes = 'BadGateway', 'BadGatewayConnection', 'GatewayTimeout', 'InternalServerError',
'RequestTimeout', 'ServerBusy', 'ServerTimeout', 'ServiceUnavailable', 'TooManyRequests'

<#
.SYNOPSIS
    Seconds to wait before retrying a request that failed with -ErrorRecord
    on attempt -Attempt, or $null when the failure is not transient.

.DESCRIPTION
    Transient: no response (the connection failed or timed out), a status in
    $script:TransientStatusCodes, or another 5xx whose error.code is in
    $script:TransientErrorCodes. Other 4xx are not. The wait is Retry-After
    when the response has it, else -BackoffSeconds doubled per attempt; both
    are capped at -MaxDelaySeconds.
#>
function Get-SuE2ETransientRetryDelay {
    param(
        [Parameter(Mandatory)][System.Management.Automation.ErrorRecord]$ErrorRecord,
        [Parameter(Mandatory)][int]$Attempt,
        [int]$BackoffSeconds = 2,
        [int]$MaxDelaySeconds = 60
    )
    $r = $ErrorRecord.Exception.PSObject.Properties['Response'] ? $ErrorRecord.Exception.Response : $null
    if (-not $r) {
        # A connection failure, or a client-side timeout (TaskCanceledException over TimeoutException).
        $e = $ErrorRecord.Exception
        $timedOut = $e -is [System.TimeoutException] -or $e.InnerException -is [System.TimeoutException]
        if ($e -isnot [System.Net.Http.HttpRequestException] -and -not $timedOut) { return $null }
    }
    elseif ([int]$r.StatusCode -notin $script:TransientStatusCodes) {
        if ([int]$r.StatusCode -lt 500) { return $null }
        $code = try { ($ErrorRecord.ErrorDetails.Message | ConvertFrom-Json -AsHashtable).error.code } catch { $null }
        if ($code -notin $script:TransientErrorCodes) { return $null }
    }
    $delay = $BackoffSeconds * [math]::Pow(2, $Attempt - 1)
    $after = if ($r) { $r.Headers.RetryAfter } else { $null }
    if ($after -and $null -ne $after.Delta) { $delay = $after.Delta.TotalSeconds }
    elseif ($after -and $null -ne $after.Date) { $delay = ($after.Date - [DateTimeOffset]::UtcNow).TotalSeconds }
    return [int][math]::Ceiling([math]::Min([math]::Max($delay, 0), $MaxDelaySeconds))
}

<#
.SYNOPSIS
    Sends one request with a token for -Resource fetched for each attempt.

.DESCRIPTION
    A 401 invalid_token (a token replaced as it expired) is retried once,
    after -RetrySeconds, with a new token. A transient failure (see
    Get-SuE2ETransientRetryDelay) is retried after its delay, up to
    -Attempts requests in all. Anything else is thrown at once.
#>
function Invoke-SuE2ERest {
    param(
        [Parameter(Mandatory)][string]$Method,
        [Parameter(Mandatory)][string]$Uri,
        [Parameter(Mandatory)][string]$Resource,
        [object]$Body,
        [int]$RetrySeconds = 30,
        [int]$Attempts = 5
    )
    $params = @{
        Method                  = $Method
        Uri                     = $Uri
        ContentType             = 'application/json'
        ResponseHeadersVariable = 'headers'
        StatusCodeVariable      = 'status'
    }
    if ($null -ne $Body) { $params.Body = ($Body | ConvertTo-Json -Depth 20) }
    $request = "$Method $($Uri -replace '\?.*')"
    $tokenRenewed = $false
    for ($attempt = 1; ; $attempt++) {
        $params.Headers = @{ Authorization = "Bearer $(Get-SuE2EToken $Resource)" }
        try {
            $response = Invoke-RestMethod @params
            return [pscustomobject]@{ Status = $status; Headers = $headers; Body = $response }
        }
        catch {
            if ($attempt -ge $Attempts) { throw }
            $r = $_.Exception.PSObject.Properties['Response'] ? $_.Exception.Response : $null
            $invalidToken = $r -and [int]$r.StatusCode -eq 401 -and "$($r.Headers.WwwAuthenticate)" -match 'invalid_token'
            if ($invalidToken -and -not $tokenRenewed) {
                $tokenRenewed = $true
                Write-Warning "401 invalid_token on $request; retrying with a new token"
                Start-Sleep -Seconds $RetrySeconds
                continue
            }
            $delay = Get-SuE2ETransientRetryDelay -ErrorRecord $_ -Attempt $attempt
            if ($null -eq $delay) { throw }
            $received = if ($r) { [int]$r.StatusCode } else { 'no response' }
            # One line, bounded: a gateway may answer with a whole HTML page.
            $reason = ("$_" -replace '\s+', ' ').Trim()
            if ($reason.Length -gt 300) { $reason = "$($reason.Substring(0, 300))..." }
            Write-Warning "$received on $request (attempt $attempt of $Attempts): $reason; retrying in $delay s"
            Start-Sleep -Seconds $delay
        }
    }
}

# GET that returns the body, or $null on 404.
function Get-SuE2EResource([string]$Uri, [string]$Resource) {
    try { return (Invoke-SuE2ERest -Method Get -Uri $Uri -Resource $Resource).Body }
    catch {
        $r = $_.Exception.PSObject.Properties['Response'] ? $_.Exception.Response : $null
        if ($r -and [int]$r.StatusCode -eq 404) { return $null }
        throw
    }
}

# Storage blob name Import-SuE2EUpdate gives one of an update's files.
function Get-SuE2EUpdateBlobName([string]$Provider, [string]$Name, [string]$Version, [string]$FileName) {
    return "$Provider/$Name/$Version/$FileName"
}

<#
.SYNOPSIS
    Waits for the long-running operation a response started, if it started
    one; throws unless it ends in success.
#>
function Wait-SuE2EOperation {
    param(
        [Parameter(Mandatory)][object]$Response,
        [Parameter(Mandatory)][string]$Resource,
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
        $r = Invoke-SuE2ERest -Method Get -Uri $uri -Resource $Resource
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
        $blob = Get-SuE2EUpdateBlobName $Provider $Name $Version (Split-Path -Leaf $file)
        az storage blob upload --auth-mode login --account-name $Config.STORAGE_ACCOUNT `
            --container-name $Config.STORAGE_CONTAINER --name $blob --file $file --overwrite --only-show-errors | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Upload of $blob failed" }
        if (-not $Blobs.Contains($blob)) { $Blobs.Add($blob) }
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
    $base = "https://$($Config.ADU_ENDPOINT)"
    $r = Invoke-SuE2ERest -Method Post -Uri "$base/updates:import?api-version=$script:ApiVersion" -Resource $script:AduScope -Body $body
    $operation = @($r.Headers['Operation-Location'])[0]
    if (-not $operation) { throw 'Import returned no Operation-Location' }
    if ($operation -notmatch '^https?://') { $operation = "$base$operation" }

    $deadline = (Get-Date).AddMinutes(10)
    while ($true) {
        $op = (Invoke-SuE2ERest -Method Get -Uri $operation -Resource $script:AduScope).Body
        if ($op.status -eq 'Succeeded') { break }
        if ($op.status -eq 'Failed') { throw "Import of $Provider/$Name/$Version failed: $($op | ConvertTo-Json -Depth 10 -Compress)" }
        if ((Get-Date) -gt $deadline) { throw "Import of $Provider/$Name/$Version did not finish in time" }
        Start-Sleep -Seconds 10
    }
    return "updates/providers/$Provider/names/$Name/versions/$Version"
}

<#
.SYNOPSIS
    Creates an OnboardingUpdate job and schedules one run of it, which starts
    now. Returns the run name. A job offers nothing until it has a run.
#>
function New-SuE2EOnboardingJob {
    param(
        [Parameter(Mandatory)][hashtable]$Config,
        [Parameter(Mandatory)][string]$JobName,
        [Parameter(Mandatory)][string]$UpdateResourceId
    )
    $uri = "$(Get-SuE2EJobsUri $Config)/$($JobName)?api-version=$script:ApiVersion"
    $body = @{
        location   = $Config.LOCATION
        properties = @{
            jobType     = 'OnboardingUpdate'
            description = 'Software updates e2e offer.'
            definition  = @{ schedulingType = 'Continuous'; updateResourceId = $UpdateResourceId }
        }
    }
    Invoke-SuE2ERest -Method Put -Uri $uri -Resource $script:ArmScope -Body $body | Out-Null

    $deadline = (Get-Date).AddMinutes(5)
    while ($true) {
        $job = (Invoke-SuE2ERest -Method Get -Uri $uri -Resource $script:ArmScope).Body
        $state = $job.properties.provisioningState
        if ($state -eq 'Succeeded') { break }
        if ($state -in 'Failed', 'Canceled') { throw "Job $JobName provisioning $state" }
        if ((Get-Date) -gt $deadline) { throw "Job $JobName did not provision in time" }
        Start-Sleep -Seconds 10
    }

    return New-SuE2EJobRun -Config $Config -JobName $JobName
}

<#
.SYNOPSIS
    Schedules a run of a job that starts now (no scheduledTime) and returns its
    name.
#>
function New-SuE2EJobRun {
    param(
        [Parameter(Mandatory)][hashtable]$Config,
        [Parameter(Mandatory)][string]$JobName
    )
    # The suffix keeps calls within the same second apart.
    $run = "run-$((Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmss'))-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
    $runUri = "$(Get-SuE2EJobsUri $Config)/$JobName/runs/$($run)?api-version=$script:ApiVersion"
    Invoke-SuE2ERest -Method Put -Uri $runUri -Resource $script:ArmScope -Body @{ properties = @{} } | Out-Null
    return $run
}

# StrictMode-safe read of a run's properties.<Name>, or $null.
function Get-SuE2ERunProperty([object]$Run, [string]$Name) {
    if (-not $Run.PSObject.Properties['properties']) { return $null }
    $p = $Run.properties.PSObject.Properties[$Name]
    if ($p) { return $p.Value }
    return $null
}

# The job's runs, newest startTime first (none: empty).
function Get-SuE2EJobRuns([hashtable]$Config, [string]$JobName) {
    $runs = @()
    $uri = "$(Get-SuE2EJobsUri $Config)/$JobName/runs?api-version=$script:ApiVersion"
    while ($uri) {
        $page = Get-SuE2EResource $uri $script:ArmScope
        if (-not $page) { break }
        $runs += @($page.value)
        # Paged by nextLink.
        $uri = if ($page.PSObject.Properties['nextLink']) { $page.nextLink } else { $null }
    }
    return @($runs | Sort-Object {
        $t = Get-SuE2ERunProperty $_ 'startTime'
        if ($t) { ([datetime]$t).ToUniversalTime() } else { [datetime]::MinValue }
    } -Descending)
}

# The newest Active run of the job, or $null.
function Get-SuE2EActiveRun([hashtable]$Config, [string]$JobName) {
    return Get-SuE2EJobRuns $Config $JobName | Where-Object { (Get-SuE2ERunProperty $_ 'status') -eq 'Active' } |
        Select-Object -First 1
}

<#
.SYNOPSIS
    Waits until every job's run is Active, then -GraceSeconds more: devices
    were measured to get 401000 for about 3 min after a run turned Active.
#>
function Wait-SuE2EJobRuns {
    param(
        [Parameter(Mandatory)][hashtable]$Config,
        [Parameter(Mandatory)][hashtable[]]$Runs,   # @{ Job; Run }
        [int]$Minutes = 10,
        [int]$GraceSeconds = 300
    )
    $deadline = (Get-Date).AddMinutes($Minutes)
    foreach ($r in $Runs) {
        $uri = "$(Get-SuE2EJobsUri $Config)/$($r.Job)/runs/$($r.Run)?api-version=$script:ApiVersion"
        while ($true) {
            $p = (Invoke-SuE2ERest -Method Get -Uri $uri -Resource $script:ArmScope).Body.properties
            $status = if ($p.PSObject.Properties['status']) { $p.status } else { '<none>' }
            if ($status -eq 'Active') { break }
            if ($status -in 'Failed', 'Canceled', 'Completed') { throw "Run $($r.Job)/$($r.Run) is $status" }
            if ((Get-Date) -gt $deadline) { throw "Run $($r.Job)/$($r.Run) not Active in $Minutes min (status $status)" }
            Start-Sleep -Seconds 10
        }
    }
    Start-Sleep -Seconds $GraceSeconds
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
    $job = "$(Get-SuE2EJobsUri $Config)/$JobName"
    # A Continuous onboarding job records results on its runs (measured). A
    # reused job may hold several, so every Active run is searched, newest
    # first, then the newest run whatever its status.
    $all = @(Get-SuE2EJobRuns $Config $JobName)
    if (-not $all) { return $null }
    $candidates = @($all | Where-Object { (Get-SuE2ERunProperty $_ 'status') -eq 'Active' })
    if ($all[0].name -notin @($candidates | ForEach-Object name)) { $candidates += $all[0] }
    foreach ($run in $candidates) {
        $uri = "$job/runs/$($run.name)/listResults?api-version=$script:ApiVersion"
        $body = @{}
        while ($true) {
            $page = (Invoke-SuE2ERest -Method Post -Uri $uri -Resource $script:ArmScope -Body $body).Body
            $hit = @($page.value) | Where-Object {
                $_.PSObject.Properties['resourceExternalId'] -and $_.resourceExternalId -eq $RegistrationId
            } | Select-Object -First 1
            if ($hit) { return $hit }
            # Paged by a skipToken sent back in the next request body.
            $next = if ($page.PSObject.Properties['skipToken']) { $page.skipToken } else { $null }
            if (-not $next) { break }
            $body = @{ skipToken = $next }
        }
    }
    return $null
}

<#
.SYNOPSIS
    Names of the offers staged under -RunId: the manufacturer, and per scenario
    its model, update and job. No remote call.
#>
function Get-SuE2EOfferNames {
    param([Parameter(Mandatory)][string]$RunId)
    $tag = ($RunId -replace '[^A-Za-z0-9]', '').ToLowerInvariant()
    $offers = [ordered]@{}
    foreach ($scenario in $script:Scenarios.Keys) {
        # Short names: the whole offer must fit the client's 4 KiB request buffer.
        $short = $script:Scenarios[$scenario].Short
        $model = "e2e-$tag-$short"
        $offers[$scenario] = [ordered]@{
            Model  = $model
            Update = "updates/providers/sdke2e/names/$model/versions/1.0.0"
            Job    = "su-e2e-$tag-$short"
        }
    }
    return [ordered]@{ Manufacturer = "e2e$tag"; Offers = $offers }
}

<#
.SYNOPSIS
    Short hash of the code that defines the staged offers (names, update
    content, job). Appended to a reused -RunId, a fixture change stages a new
    offer set instead of reusing one staged by older code.
#>
function Get-SuE2EFixtureTag {
    $text = ('Get-SuE2EOfferNames', 'Import-SuE2EUpdate', 'New-SuE2EOnboardingJob', 'New-SuE2EJobRun' |
            ForEach-Object { (Get-Command -Module SuE2E -Name $_).Definition }) -join "`n"
    $text += ($script:Scenarios.Keys | ForEach-Object { "$_=$($script:Scenarios[$_].Short)" }) -join ','
    $hash = [System.Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($text -replace "`r`n", "`n")))
    return [Convert]::ToHexString($hash).Substring(0, 8).ToLowerInvariant()
}

<#
.SYNOPSIS
    Exports the variables the device suites read for the offers staged under
    -RunId (also to $env:GITHUB_ENV when set). No remote call.
#>
function Export-SuE2EOfferVariables {
    param([Parameter(Mandatory)][string]$RunId)
    $names = Get-SuE2EOfferNames -RunId $RunId
    $exports = [ordered]@{ AZ_IOT_E2E_SU_OFFER_MANUFACTURER = $names.Manufacturer }
    foreach ($scenario in $names.Offers.Keys) { $exports[$script:Scenarios[$scenario].Variable] = $names.Offers[$scenario].Model }
    foreach ($k in $exports.Keys) {
        Set-Item -Path "env:$k" -Value $exports[$k]
        if ($env:GITHUB_ENV) { Add-Content -Path $env:GITHUB_ENV -Value "$k=$($exports[$k])" }
    }
}

<#
.SYNOPSIS
    Stages one offer per scenario, records them in -StatePath, and exports the
    variables the device suites read (also to $env:GITHUB_ENV when set).

.DESCRIPTION
    With -Reuse, offers already staged under -RunId are kept: an existing
    update is not imported again, an existing job is not recreated, and a job
    with an Active run gets no new run. A run another stager scheduled first
    is waited for like a new one. Only new runs are waited for. An offer
    reaches each device once, so reused offers serve any number of new devices.
#>
function New-SuE2EOffers {
    param(
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$StatePath,
        [string]$WorkDir = (Join-Path ([System.IO.Path]::GetTempPath()) "su-e2e-$RunId"),
        # Wait after the runs turn Active; see Wait-SuE2EJobRuns.
        [int]$GraceSeconds = 300,
        [switch]$Reuse
    )
    $config = Get-SuE2EConfig
    $names = Get-SuE2EOfferNames -RunId $RunId
    $manufacturer = $names.Manufacturer
    $runs = @()
    $graceUntil = [datetime]::MinValue
    $state = [ordered]@{ Manufacturer = $manufacturer; Offers = [ordered]@{} }

    # Saved before every remote call, so a run killed mid-step still leaves
    # Remove-SuE2EOffers everything it may have created.
    $save = { $state | ConvertTo-Json -Depth 5 | Set-Content -Path $StatePath }

    # With -Reuse every offer may already exist, so all names are recorded
    # before the first lookup; cleanup skips what does not exist.
    foreach ($scenario in $names.Offers.Keys) {
        $n = $names.Offers[$scenario]
        $blobs = [System.Collections.Generic.List[string]]::new()
        if ($Reuse) {
            foreach ($file in "$($n.Model).bin", "$($n.Model).importmanifest.json") {
                $blobs.Add((Get-SuE2EUpdateBlobName 'sdke2e' $n.Model '1.0.0' $file))
            }
        }
        $state.Offers[$scenario] = [ordered]@{
            Model  = $n.Model
            Blobs  = $blobs
            Update = $n.Update
            Job    = $Reuse ? $n.Job : $null
            Run    = $null
        }
    }
    & $save

    foreach ($scenario in $names.Offers.Keys) {
        $n = $names.Offers[$scenario]
        $offer = $state.Offers[$scenario]

        $updateUri = "https://$($config.ADU_ENDPOINT)/$($n.Update)?api-version=$script:ApiVersion"
        if ($Reuse -and (Get-SuE2EResource $updateUri $script:AduScope)) {
            Write-Host "$scenario`: update $($n.Model) kept"
        }
        else {
            # Concurrent stagers of one -RunId overwrite each other's blobs, so
            # an import can fail while another succeeds: re-check, then retry.
            for ($attempt = 1; ; $attempt++) {
                try {
                    [void](Import-SuE2EUpdate -Config $config -Provider 'sdke2e' -Name $n.Model `
                            -Version '1.0.0' -Manufacturer $manufacturer -Model $n.Model -WorkDir $WorkDir `
                            -Blobs $offer.Blobs -OnChange $save)
                    break
                }
                catch {
                    if (-not $Reuse) { throw }
                    if (Get-SuE2EResource $updateUri $script:AduScope) {
                        Write-Host "$scenario`: update $($n.Model) imported by another run"
                        break
                    }
                    if ($attempt -ge 3) { throw }
                    Write-Warning "$scenario`: import attempt $attempt failed, retrying: $_"
                }
            }
        }

        $offer.Job = $n.Job
        & $save
        $jobUri = "$(Get-SuE2EJobsUri $config)/$($n.Job)?api-version=$script:ApiVersion"
        $job = if ($Reuse) { Get-SuE2EResource $jobUri $script:ArmScope } else { $null }
        try {
            if ($job -and $job.properties.provisioningState -eq 'Succeeded') {
                $active = Get-SuE2EActiveRun $config $n.Job
                if ($active) {
                    $offer.Run = $active.name
                    Write-Host "$scenario`: job $($n.Job) kept, run $($offer.Run) Active"
                    # Another run may have just created it: wait out what is left of its grace.
                    $started = Get-SuE2ERunProperty $active 'startTime'
                    if ($started) {
                        $until = ([datetime]$started).ToUniversalTime().AddSeconds($GraceSeconds)
                        if ($until -gt $graceUntil) { $graceUntil = $until }
                    }
                }
                else { $offer.Run = New-SuE2EJobRun -Config $config -JobName $n.Job; $runs += @{ Job = $n.Job; Run = $offer.Run } }
            }
            else {
                $offer.Run = New-SuE2EOnboardingJob -Config $config -JobName $n.Job -UpdateResourceId $n.Update
                $runs += @{ Job = $n.Job; Run = $offer.Run }
            }
        }
        catch {
            # A concurrent stager of the same job scheduled a run after our lookup: wait for that run.
            if (-not $Reuse -or "$_" -notmatch 'JobRunAlreadyActive') { throw }
            $other = Get-SuE2EJobRuns $config $n.Job |
                Where-Object { (Get-SuE2ERunProperty $_ 'status') -notin 'Failed', 'Canceled', 'Completed' } |
                Select-Object -First 1
            if (-not $other) { throw }
            $offer.Run = $other.name
            Write-Host "$scenario`: job $($n.Job) run $($offer.Run) scheduled by another run"
            $runs += @{ Job = $n.Job; Run = $offer.Run }
        }
        & $save
    }
    if ($runs.Count) { Wait-SuE2EJobRuns -Config $config -Runs $runs -GraceSeconds $GraceSeconds }
    else {
        # Measured after every lookup, so their time counts toward the grace.
        $left = [math]::Ceiling(($graceUntil - (Get-Date).ToUniversalTime()).TotalSeconds)
        if ($left -gt 0) {
            Write-Host "Waiting $left s: a kept run started less than $GraceSeconds s ago"
            Start-Sleep -Seconds $left
        }
    }
    Export-SuE2EOfferVariables -RunId $RunId
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
                        $r = Invoke-SuE2ERest -Method Delete -Resource $script:ArmScope `
                            -Uri "$(Get-SuE2EJobsUri $config)/$($name)?api-version=$script:ApiVersion"
                        Wait-SuE2EOperation -Response $r -Resource $script:ArmScope -BaseUri $config.ARM_ENDPOINT
                    }
                    'Update' {
                        $r = Invoke-SuE2ERest -Method Delete -Resource $script:AduScope `
                            -Uri "https://$($config.ADU_ENDPOINT)/$($name)?api-version=$script:ApiVersion"
                        Wait-SuE2EOperation -Response $r -Resource $script:AduScope -BaseUri "https://$($config.ADU_ENDPOINT)"
                    }
                    'Blob' {
                        # Recorded for a reused update, so it may be gone already.
                        $exists = az storage blob exists --auth-mode login --account-name $config.STORAGE_ACCOUNT `
                            --container-name $config.STORAGE_CONTAINER --name $name --query exists -o tsv
                        if ($LASTEXITCODE -ne 0) { throw "Existence check of blob $name failed" }
                        if ($exists -ne 'true') { continue }
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
New-SuE2EOnboardingJob, New-SuE2EJobRun, Wait-SuE2EJobRuns, Get-SuE2EDeviceResult, Get-SuE2EOfferNames,
Export-SuE2EOfferVariables, Get-SuE2EFixtureTag
