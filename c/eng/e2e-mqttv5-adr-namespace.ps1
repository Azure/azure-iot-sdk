# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

<#
.SYNOPSIS
Ensures the ADR namespace of the shared mqttv5 e2e environment exists and links its hub and DPS.

.DESCRIPTION
Workaround: in some regions the ADR namespace can disappear from ARM while the ADR service still
holds it; its role assignments go with it, and DPS registrations then fail. When the namespace
is missing, or an endpoint is not linked, this script:
  1. re-creates it (same name/region; if ADR still holds it, re-asserts its endpoints),
  2. re-grants the 8 role assignments the link needs (idempotent),
  3. links the hub (messaging/hub-1) and DPS (provisioning/dps-1) and waits for Succeeded,
  4. pushes the namespace to the DPS data plane (tags-only DPS update).
If the namespace is present with both endpoints Succeeded it makes one ARM read and exits.

Requires `az` logged in with, on the resource group: Contributor, and
Microsoft.Authorization/roleAssignments/write (e.g. Role Based Access Control Administrator)
for Contributor (b24988ac-...), IoT Hub Data Contributor (4fc6c259-...) and
Azure Device Registry Contributor (a5c3590a-...).
Hub and DPS must have system-assigned identities.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory)][string]$SubscriptionId,
    [Parameter(Mandatory)][string]$ResourceGroup,
    [Parameter(Mandatory)][string]$HubName,
    [Parameter(Mandatory)][string]$DpsName,
    [Parameter(Mandatory)][string]$NamespaceName,
    [string]$AdrApiVersion = '2026-11-02-preview',
    [string]$HubApiVersion = '2026-10-01-preview',
    # Empty: the first of 2026-11-02-preview / 2026-03-01-preview that ARM serves, else its newest.
    [string]$DpsApiVersion = '',
    [int]$LinkTimeoutMinutes = 12,
    [int]$LinkAttempts = 4
)

$ErrorActionPreference = 'Stop'

$Arm = 'https://management.azure.com'
$Rg = "/subscriptions/$SubscriptionId/resourceGroups/$ResourceGroup"
$HubId = "$Rg/providers/Microsoft.Devices/IotHubs/$HubName"
$DpsId = "$Rg/providers/Microsoft.Devices/provisioningServices/$DpsName"
$NsId = "$Rg/providers/Microsoft.DeviceRegistry/namespaces/$NamespaceName"
$HubUrl = "$Arm${HubId}?api-version=$HubApiVersion"
$NsUrl = "$Arm${NsId}?api-version=$AdrApiVersion"
$RoleContributor = 'b24988ac-6180-42a0-ab88-20f7382dd24c'
$RoleHubData = '4fc6c259-987e-4a07-842e-c321cc9d413f'
$RoleAdr = 'a5c3590a-3a1a-4cd4-9648-ea0a32b15137'
# Link failures that clear once new role assignments replicate.
$PropagationCodes = 'AdrMiNotAuthorized|LinkableResourceNotReady|AuthorizationFailed|LinkInitiateFailed|NamespaceMiTokenAcquisitionFailed|OutboundIdentityUnavailable'
# Link submissions that may be rejected outright yet succeed on a later attempt.
$RetryableSubmit = "$PropagationCodes|ResourceProvisioningInProgress|Service ?Unavailable|Gateway ?Timeout|Bad ?Gateway|Too ?Many ?Requests|Internal ?Server ?Error|ServerTimeout|ServerBusy"
# ADR feature flag: without it the link takes another path and reports the linked resource as unreadable.
$FeatureTags = @{ useMiSdk = 'true' }

# Returns $null on 404. Response bodies are not logged: DPS reads can carry keys.
function Invoke-Arm([string]$Method, [string]$Url, $Body) {
    $a = @('rest', '--method', $Method, '--url', $Url, '-o', 'json')
    $tmp = $null
    if ($null -ne $Body) {
        $tmp = New-TemporaryFile
        $Body | ConvertTo-Json -Depth 20 -Compress | Set-Content -LiteralPath $tmp -Encoding utf8NoBOM
        $a += @('--headers', 'Content-Type=application/json', '--body', "@$tmp")
    }
    try {
        $out = ((& az @a 2>&1) | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) {
            if ($Method -eq 'get' -and $out -match 'ResourceNotFound|\(NotFound\)|Not Found') { return $null }
            $msg = $out -replace '(?i)("(?:primaryKey|secondaryKey|accessToken)"\s*:\s*")[^"]*', '$1<redacted>'
            throw "ARM $($Method.ToUpper()) $($Url -replace '\?.*') failed: $msg"
        }
        if ($out) { $out | ConvertFrom-Json -Depth 50 }
    }
    finally { if ($tmp) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue } }
}

function Get-EndpointStates($Ns) {
    foreach ($g in 'messaging', 'provisioning') {
        $eps = $Ns.properties.$g.endpoints
        if (-not $eps) { continue }
        foreach ($p in $eps.PSObject.Properties) {
            [pscustomobject]@{ Name = "$g/$($p.Name)"; State = [string]$p.Value.linkingState; Code = [string]$p.Value.linkingError.code; Message = [string]$p.Value.linkingError.message }
        }
    }
}

# hub-1 and dps-1 Succeeded and pointing at this hub and DPS.
function Test-EndpointsLinked($Ns) {
    if (-not $Ns) { return $false }
    foreach ($e in @(@($Ns.properties.messaging.endpoints.'hub-1', $HubId), @($Ns.properties.provisioning.endpoints.'dps-1', $DpsId))) {
        if (-not $e[0] -or $e[0].linkingState -ne 'Succeeded') { return $false }
        if ($e[0].resourceId -and $e[0].resourceId -ne $e[1]) { return $false }
    }
    $true
}

function Test-Linked($Ns) { $Ns -and $Ns.properties.provisioningState -eq 'Succeeded' -and (Test-EndpointsLinked $Ns) }

function Get-NsTags($Ns) {
    $t = @{}
    if ($Ns -and $Ns.tags) { $Ns.tags.PSObject.Properties | ForEach-Object { $t[$_.Name] = $_.Value } }
    $FeatureTags.GetEnumerator() | ForEach-Object { $t[$_.Key] = $_.Value }
    $t
}

function Wait-For([string]$What, [int]$Minutes, [scriptblock]$Probe) {
    $deadline = (Get-Date).AddMinutes($Minutes)
    while ($true) {
        $r = & $Probe
        if ($r.Done) { return $r }
        if ($r.ContainsKey('Failed') -and $r.Failed) { throw "$What failed: $($r.Detail)" }
        if ((Get-Date) -ge $deadline) { throw "$What did not finish within $Minutes min (last: $($r.Detail))." }
        Write-Host "  $What : $($r.Detail)"
        Start-Sleep -Seconds 15
    }
}

function Grant-Role([string]$Scope, [string]$PrincipalId, [string]$RoleId, [string]$What) {
    for ($i = 1; $i -le 8; $i++) {
        $out = ((& az role assignment create --assignee-object-id $PrincipalId --assignee-principal-type ServicePrincipal --role $RoleId --scope $Scope -o none 2>&1) | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -or $out -match 'RoleAssignmentExists|already exists') { Write-Host "  role: $What"; return }
        if ($i -eq 8) { throw "Role assignment '$What' failed: $out" }
        Start-Sleep -Seconds 15
    }
}

[void](& az account set --subscription $SubscriptionId)
if ($LASTEXITCODE -ne 0) { throw "az account set --subscription $SubscriptionId failed." }

$ns = Invoke-Arm get $NsUrl
if (Test-Linked $ns) { Write-Host "ADR namespace $NamespaceName present and linked."; return }
Write-Host "::warning::ADR namespace $NamespaceName $(if ($ns) { 'not linked' } else { 'missing from ARM' }); rebuilding it."

if (-not $DpsApiVersion) {
    $served = @(((& az provider show --namespace Microsoft.Devices -o json 2>$null) | Out-String | ConvertFrom-Json).resourceTypes |
        Where-Object resourceType -eq 'provisioningServices' | ForEach-Object apiVersions)
    if (-not $served) { throw 'Could not read the Microsoft.Devices/provisioningServices api-versions.' }
    $DpsApiVersion = @(@('2026-11-02-preview', '2026-03-01-preview') | Where-Object { $_ -in $served }) + @($served | Sort-Object -Descending) | Select-Object -First 1
}
Write-Host "  DPS api-version: $DpsApiVersion"
$DpsUrl = "$Arm${DpsId}?api-version=$DpsApiVersion"
$hub = Invoke-Arm get $HubUrl
$dps = Invoke-Arm get $DpsUrl
if (-not $hub -or -not $dps) { throw 'Hub or DPS not found.' }
$hubPid = $hub.identity.principalId; $dpsPid = $dps.identity.principalId
if (-not $hubPid -or -not $dpsPid) { throw 'Hub and DPS need system-assigned identities.' }
# ADR requires the namespace in the hub's region.
$location = $hub.location

# Full PUTs replace tags: keep existing ones, re-assert the feature flag.
$tags = Get-NsTags $ns
$tags['restoredBy'] = 'e2e-mqttv5-adr-namespace'
$linkBody = @{
    location = $location; identity = @{ type = 'SystemAssigned' }; tags = $tags
    properties = @{
        messaging    = @{ endpoints = @{ 'hub-1' = @{ endpointType = 'Microsoft.Devices/IotHubs'; resourceId = $HubId
                        inboundCallerIdentity = @{ type = 'SystemAssigned' }; provisioning = @{ availability = 'Available'; allocationWeight = 1 } } } }
        provisioning = @{ endpoints = @{ 'dps-1' = @{ endpointType = 'Microsoft.Devices/provisioningServices'; resourceId = $DpsId
                        inboundCallerIdentity = @{ type = 'SystemAssigned' } } } }
    }
}

# 1. Namespace. A bare create fails with EndpointUsedByNestedDevices when ADR still holds it; then re-assert its endpoints.
if (-not $ns) {
    try { [void](Invoke-Arm put $NsUrl @{ location = $location; identity = @{ type = 'SystemAssigned' }; tags = $tags }) }
    catch {
        if ($_.Exception.Message -notmatch 'EndpointUsedByNestedDevices') { throw }
        Write-Host '  ADR still holds the namespace; re-asserting its endpoints.'
        [void](Invoke-Arm put $NsUrl $linkBody)
    }
}
$ns = (Wait-For 'namespace' 10 {
        $n = Invoke-Arm get $NsUrl
        $ps = if ($n) { $n.properties.provisioningState } else { 'absent' }
        @{ Done = ($ps -in 'Succeeded', 'Failed' -and $n.identity.principalId); Detail = $ps; Ns = $n } }).Ns
$nsPid = $ns.identity.principalId

# 2. Role assignments (removed with the namespace; the namespace identity may be new).
Grant-Role $HubId $nsPid $RoleContributor 'namespace -> hub (Contributor)'
Grant-Role $NsId $hubPid $RoleContributor 'hub -> namespace (Contributor)'
Grant-Role $DpsId $nsPid $RoleContributor 'namespace -> DPS (Contributor)'
Grant-Role $NsId $dpsPid $RoleContributor 'DPS -> namespace (Contributor)'
Grant-Role $NsId $dpsPid $RoleAdr 'DPS -> namespace (Azure Device Registry Contributor)'
Grant-Role $HubId $dpsPid $RoleHubData 'DPS -> hub (IoT Hub Data Contributor)'
Grant-Role $NsId $nsPid $RoleContributor 'namespace -> namespace (Contributor)'
Grant-Role $HubId $nsPid $RoleHubData 'namespace -> hub (IoT Hub Data Contributor)'

# 3. Link hub + DPS in one write (ADR refuses a messaging endpoint alone).
for ($attempt = 1; ; $attempt++) {
    Start-Sleep -Seconds 60   # role assignment replication
    if ($attempt -gt 1) {
        # ARM rejects writes (ResourceProvisioningInProgress) until a failed link settles.
        try { [void](Wait-For 'namespace settle' 5 {
                    $ps = (Invoke-Arm get $NsUrl).properties.provisioningState
                    @{ Done = (-not $ps -or $ps -in 'Succeeded', 'Failed', 'Canceled'); Detail = $ps } }) }
        catch { Write-Host "  $($_.Exception.Message)" }
    }
    Write-Host "  link attempt $attempt/$LinkAttempts"
    try { [void](Invoke-Arm put $NsUrl $linkBody) }
    catch {
        $msg = $_.Exception.Message
        if ($msg -notmatch $RetryableSubmit -or $attempt -ge $LinkAttempts) { throw }
        Write-Host "  link rejected, retrying: $msg"
        continue
    }
    $r = Wait-For 'link' $LinkTimeoutMinutes {
        $n = Invoke-Arm get $NsUrl
        $s = @(if ($n) { Get-EndpointStates $n })
        $failed = @($s | Where-Object State -eq 'Failed')
        $done = (Test-Linked $n) -or $failed.Count -gt 0 -or ($n -and $n.properties.provisioningState -in 'Failed', 'Canceled')
        @{ Done = $done; Linked = (Test-Linked $n); Failed = $false; States = $failed; Ns = $n
           Detail = if ($s) { ($s | ForEach-Object { "$($_.Name)=$($_.State)" }) -join ', ' } else { '<no endpoints>' } } }
    if ($r.Linked) { break }
    # Endpoints linked but namespace Failed: a tags-only update re-runs reconciliation
    # (re-sending endpoints is rejected as immutable). Failed can persist briefly after it is accepted.
    if ((Test-EndpointsLinked $r.Ns) -and $r.Ns.properties.provisioningState -eq 'Failed') {
        Write-Host '  endpoints linked, namespace Failed; reconciling (tags-only update)'
        $t = Get-NsTags $r.Ns
        $t['adrReconcile'] = [guid]::NewGuid().ToString('N')
        [void](Invoke-Arm patch $NsUrl @{ tags = $t })
        [void](Wait-For 'namespace reconcile' 5 {
                $ps = (Invoke-Arm get $NsUrl).properties.provisioningState
                @{ Done = ($ps -eq 'Succeeded'); Detail = $ps } })
        break
    }
    $why = ($r.States | ForEach-Object { "$($_.Name): $($_.Code) $($_.Message)" }) -join '; '
    $retryable = $r.States.Count -gt 0 -and -not ($r.States | Where-Object { $_.Code -notmatch "^($PropagationCodes)$" })
    if (-not $retryable -or $attempt -ge $LinkAttempts) { throw "ADR link failed: $(if ($why) { $why } else { 'namespace Failed' })" }
    Write-Host "  role replication pending ($why); retrying"
}

# 4. Push the namespace to the DPS data plane (tags-only update).
# Fresh read: recovery can take over an hour and the PATCH replaces the whole tag map.
$dps = Invoke-Arm get $DpsUrl
$dpsTags = @{}
if ($dps.tags) { $dps.tags.PSObject.Properties | ForEach-Object { $dpsTags[$_.Name] = $_.Value } }
$marker = [guid]::NewGuid().ToString('N')
$dpsTags['adrNamespaceRestored'] = $marker
[void](Invoke-Arm patch $DpsUrl @{ tags = $dpsTags })
[void](Wait-For 'DPS update' 5 {
        $d = Invoke-Arm get $DpsUrl
        # Succeeded/Active is also the pre-update state; require the new marker.
        @{ Done = ($d.tags.adrNamespaceRestored -eq $marker -and $d.properties.provisioningState -eq 'Succeeded' -and $d.properties.state -eq 'Active'); Detail = "$($d.properties.provisioningState) / $($d.properties.state)" } })

Write-Host "ADR namespace $NamespaceName rebuilt and linked."
if ($env:GITHUB_STEP_SUMMARY) { "ADR namespace ``$NamespaceName`` was rebuilt and re-linked by this run." | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY }
