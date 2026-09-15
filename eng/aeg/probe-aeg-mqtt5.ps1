<#
.SYNOPSIS
Step-by-step probe: can this subscription create an AEG-backed
(connectionProfile=mqttv5) IoT Hub, and will DPS accept one?

.DESCRIPTION
Mirrors the documented canary MQTT5 hub-creation flow, with each step reporting PASS/FAIL so a
single run tells you every blocker rather than only the first. Steps 1-3 are read-only; 4+ create
resources in a disposable resource group.

Step 9 (DPS) goes beyond that flow, which connects the device straight to the hub and never uses
DPS. It is here because the .NET client currently exposes no public direct-hub connect.

Run with an identity holding Contributor on -SubscriptionId.

.EXAMPLE
pwsh ./probe-aeg-mqtt5.ps1 -SubscriptionId <sub> -WhatIfOnly      # read-only steps
pwsh ./probe-aeg-mqtt5.ps1 -SubscriptionId <sub>                  # full run, self-cleaning
pwsh ./probe-aeg-mqtt5.ps1 -KeepResources   # leave the hub up for SDK testing
#>
[CmdletBinding()]
param(
    # Mandatory on purpose: no default, so a run can never create billable
    # resources in whichever subscription the shell happened to be pointed at.
    [Parameter(Mandatory)]
    [string]$SubscriptionId,

    # AEG/MQTT5 is canary-only: eastus2euap.
    [string]$Location       = "eastus2euap",

    # GLOBAL ARM host. The REGIONAL host is documented as unreliable:
    # (eastus2euap.management.azure.com) fails intermittently with SSL/EOF.
    [string]$ArmEndpoint    = "https://management.azure.com",

    # The control-plane api-version carrying connectionProfile.
    [string]$HubApiVersion  = "2026-08-01-preview",

    [string]$ResourceGroup  = "aeg-mqtt5-probe-$([guid]::NewGuid().ToString('N').Substring(0,8))",
    [string]$HubName        = "mqtt5bb-$([guid]::NewGuid().ToString('N').Substring(0,12))",
    [string]$Sku            = "S1",

    # Bug bash section 4 says the hub reaches Succeeded in ~7-9 minutes.
    [int]$CreateTimeoutMinutes = 25,

    [switch]$WhatIfOnly,
    [switch]$KeepResources,
    [switch]$SkipDps,

    # The documented create body. When this file is present it is used verbatim
    # (with location patched to -Location), so the probe sends exactly what the bug
    # bash sends rather than a reconstruction. The script also carries an inline
    # fallback that was verified byte-identical, so it works as a single file.
    [string]$HubBodyPath = (Join-Path $PSScriptRoot 'hub-body.json'),

    # Machine-readable result written here. Hand this file back; it is the whole
    # point of the run. Secrets are never written to it.
    [string]$ReportPath = (Join-Path (Get-Location) 'aeg-probe-report.json'),

    # Full console transcript alongside the report.
    [string]$TranscriptPath = (Join-Path (Get-Location) 'aeg-probe-transcript.txt')
)

$ErrorActionPreference = 'Continue'
$script:Results = [System.Collections.ArrayList]::new()
$script:Aborted = $false
$script:StepNote = $null
# Observed facts, written to the JSON report. NEVER put a key or connection string here.
$script:Facts = [ordered]@{}

try { Start-Transcript -Path $TranscriptPath -Force | Out-Null } catch { Write-Host "(transcript unavailable: $($_.Exception.Message))" }

# az rest cannot take a request body on stdin: '--body @-' is passed through as a
# literal filename and fails with a confusing parse error. 'az rest --help'
# documents only the '@{file}' form, so stage every body in a temp file.
function Invoke-ArmRest {
    param(
        [Parameter(Mandatory)][ValidateSet('get','post','put','patch','delete')][string]$Method,
        [Parameter(Mandatory)][string]$Url,
        [string]$JsonBody
    )
    # --resource makes az acquire an ARM-audience token.
    $azArgs = @('rest','--method',$Method,'--url',$Url,'--resource','https://management.azure.com/','-o','json')
    $tmp = $null
    if ($PSBoundParameters.ContainsKey('JsonBody') -and $JsonBody) {
        $tmp = [System.IO.Path]::GetTempFileName()
        [System.IO.File]::WriteAllText($tmp, $JsonBody)
        $azArgs += @('--headers','Content-Type=application/json','--body',"@$tmp")
    }
    try {
        $raw = (& az @azArgs 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "az rest $Method failed: $raw" }
        if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
        try { return $raw | ConvertFrom-Json }
        catch { throw "az rest $Method returned non-JSON: $raw" }
    }
    finally { if ($tmp -and (Test-Path $tmp)) { Remove-Item $tmp -Force -ErrorAction SilentlyContinue } }
}

function Step {
    param([string]$Name, [scriptblock]$Body, [switch]$Mutating, [switch]$Critical)
    Write-Host ""
    Write-Host "=== $Name" -ForegroundColor Cyan
    # Once a required step fails, downstream steps would only restate the same root
    # cause in a different error. Skip them instead of manufacturing noise.
    if ($script:Aborted) {
        Write-Host "SKIP (an earlier required step failed)" -ForegroundColor Yellow
        [void]$script:Results.Add([pscustomobject]@{ Step=$Name; Result='SKIP'; Detail='earlier required step failed' })
        return $null
    }
    if ($Mutating -and $WhatIfOnly) {
        Write-Host "SKIP (-WhatIfOnly)" -ForegroundColor Yellow
        [void]$script:Results.Add([pscustomobject]@{ Step=$Name; Result='SKIP'; Detail='-WhatIfOnly' })
        return $null
    }
    try {
        $script:StepNote = $null
        $out = & $Body
        if ($script:StepNote) {
            # The step ran but could not answer its question. Distinct from PASS so
            # an unanswered check is never mistaken for a satisfied one.
            Write-Host "INCONCLUSIVE" -ForegroundColor Yellow
            [void]$script:Results.Add([pscustomobject]@{ Step=$Name; Result='INCONCLUSIVE'; Detail=$script:StepNote })
            $script:StepNote = $null
            return $out
        }
        Write-Host "PASS" -ForegroundColor Green
        [void]$script:Results.Add([pscustomobject]@{ Step=$Name; Result='PASS'; Detail='' })
        return $out
    }
    catch {
        Write-Host "FAIL: $($_.Exception.Message)" -ForegroundColor Red
        [void]$script:Results.Add([pscustomobject]@{ Step=$Name; Result='FAIL'; Detail=$_.Exception.Message })
        if ($Critical) { $script:Aborted = $true }
        return $null
    }
}

$hubUrl = "$ArmEndpoint/subscriptions/$SubscriptionId/resourceGroups/$ResourceGroup/providers/Microsoft.Devices/IotHubs/$HubName" +
          "?api-version=$HubApiVersion"

# ------------------------------------------------------------------ 1. identity
Step -Critical "1. Azure identity and subscription" {
    $acct = az account show -o json 2>$null | ConvertFrom-Json
    if (-not $acct) { throw "Not logged in. Run 'az login' (or sign in a service principal)." }
    az account set --subscription $SubscriptionId 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Cannot select subscription $SubscriptionId." }
    $acct = az account show -o json | ConvertFrom-Json
    Write-Host "  identity : $($acct.user.name) ($($acct.user.type))"
    Write-Host "  sub      : $($acct.id)"
    if ($acct.id -ne $SubscriptionId) { throw "Active subscription is $($acct.id), expected $SubscriptionId." }
}

# ------------------------------------- 2. is that api-version offered here
Step -Critical "2. Microsoft.Devices provider + api-version $HubApiVersion" {
    $p = az provider show --namespace Microsoft.Devices -o json | ConvertFrom-Json
    Write-Host "  registrationState: $($p.registrationState)"
    if ($p.registrationState -ne 'Registered') {
        throw "Microsoft.Devices is '$($p.registrationState)'. Run: az provider register --namespace Microsoft.Devices"
    }
    $rt = $p.resourceTypes | Where-Object { $_.resourceType -eq 'IotHubs' }
    if (-not $rt) { throw "No IotHubs resource type reported." }
    $newest = $rt.apiVersions | Sort-Object -Descending | Select-Object -First 6
    Write-Host "  newest api-versions: $($newest -join ', ')"
    $script:Facts['providerRegistrationState'] = $p.registrationState
    $script:Facts['newestHubApiVersions']      = @($newest)
    $script:Facts['requestedApiVersionOffered']= [bool]($rt.apiVersions -contains $HubApiVersion)
    # NOT fatal: the provider manifest is not always a reliable guide to what a
    # canary region actually serves. Step 5 is the real test.
    if ($rt.apiVersions -notcontains $HubApiVersion) {
        Write-Host "  WARNING: $HubApiVersion not listed in the provider manifest; step 5 will tell us for real." -ForegroundColor Yellow
    }
}

# ------------------------------------------------------------ 3. name availability
Step -Critical "3. Hub name availability ($HubName)" {
    $body = @{ name = "$HubName.azure-devices.net"; type = 'Microsoft.Devices/IotHubs' } | ConvertTo-Json -Compress
    $url  = "$ArmEndpoint/subscriptions/$SubscriptionId/providers/Microsoft.Devices/checkNameAvailability?api-version=$HubApiVersion"
    $r = Invoke-ArmRest -Method post -Url $url -JsonBody $body
    if (-not $r.nameAvailable) { throw "Name not available: $($r.reason) $($r.message)" }
}

# -------------------------------------------------------------- 4. resource group
$rgCreated = Step -Mutating -Critical "4. Create resource group $ResourceGroup in $Location" {
    az group create --name $ResourceGroup --location $Location --tags purpose=mqtt5-aeg-probe -o none
    if ($LASTEXITCODE -ne 0) { throw "Resource group create failed -- '$Location' may not be enabled for this subscription." }
    $true
}

# -------------------------------- 5. THE question: does OUR sub get an mqttv5 hub?
Step -Mutating -Critical "5. PUT IoT Hub with properties.connectionProfile = mqttv5" {
    # Prefer hub-body.json, patched only for location/sku.
    if (Test-Path $HubBodyPath) {
        Write-Host "  using hub body: $HubBodyPath"
        $obj = Get-Content -Raw $HubBodyPath | ConvertFrom-Json
        $obj.location = $Location
        $obj.sku.name = $Sku
        $payload = $obj | ConvertTo-Json -Depth 20 -Compress
    }
    else {
        # Fallback reconstruction, verified byte-identical to hub-body.json.
        Write-Host "  hub-body.json not found; using the inline reconstruction." -ForegroundColor Yellow
        $payload = [ordered]@{
            location   = $Location
            sku        = [ordered]@{ name = $Sku; capacity = 1 }
            properties = [ordered]@{
                routing = [ordered]@{
                    routes = @(
                        [ordered]@{
                            name          = 'device-messages-to-events'
                            source        = 'DeviceMessages'
                            condition     = 'true'
                            endpointNames = @('events')
                            isEnabled     = $true
                        }
                    )
                }
                connectionProfile = 'mqttv5'
            }
        } | ConvertTo-Json -Depth 20 -Compress
    }

    Write-Host "  PUT $hubUrl"
    Write-Host "  body $payload"
    $script:Facts['createBodySent'] = ($payload | ConvertFrom-Json)
    $hub = Invoke-ArmRest -Method put -Url $hubUrl -JsonBody $payload
    Write-Host "  provisioningState: $($hub.properties.provisioningState)"
    $script:Facts['putAccepted'] = $true
    $hub
}

# ------------------------------------------- 6. poll, then confirm it really stuck
$hubFinal = Step -Mutating -Critical "6. Poll to Succeeded/Active and confirm connectionProfile" {
    $deadline = (Get-Date).AddMinutes($CreateTimeoutMinutes)
    $started  = Get-Date
    do {
        Start-Sleep -Seconds 30
        $hub   = Invoke-ArmRest -Method get -Url $hubUrl
        $state = $hub.properties.provisioningState
        $run   = $hub.properties.state
        Write-Host ("  [{0,5:N1}m] provisioningState={1} state={2}" -f ((Get-Date)-$started).TotalMinutes, $state, $run)
    } while ($state -in @('Accepted','Creating','Updating') -and (Get-Date) -lt $deadline)

    if ($state -ne 'Succeeded') { throw "Hub did not reach Succeeded within $CreateTimeoutMinutes min (last: $state)." }
    # Completion requires BOTH: provisioningState=Succeeded AND state=Active.
    if ($hub.properties.state -ne 'Active') { throw "provisioningState=Succeeded but state='$($hub.properties.state)', expected Active." }

    Write-Host "  connectionProfile = '$($hub.properties.connectionProfile)'"
    Write-Host "  sku               = $($hub.sku.name) / $($hub.sku.tier)"
    Write-Host "  hostName          = $($hub.properties.hostName)"
    Write-Host "  deviceHostName    = $($hub.properties.deviceHostName)"
    Write-Host "  serviceHostName   = $($hub.properties.serviceHostName)"

    $script:Facts['createMinutes']            = [math]::Round(((Get-Date)-$started).TotalMinutes,1)
    $script:Facts['provisioningState']        = $state
    $script:Facts['state']                    = $hub.properties.state
    # The single most important observation: what casing/value the service echoes.
    $script:Facts['connectionProfileEchoed']  = $hub.properties.connectionProfile
    $script:Facts['skuName']                  = $hub.sku.name
    $script:Facts['skuTier']                  = $hub.sku.tier
    $script:Facts['hostName']                 = $hub.properties.hostName
    $script:Facts['deviceHostName']           = $hub.properties.deviceHostName
    $script:Facts['serviceHostName']          = $hub.properties.serviceHostName
    $script:Facts['mqttV5Settings']           = $hub.properties.mqttV5Settings
    $script:Facts['hubPropertyNames']         = @($hub.properties.PSObject.Properties.Name | Sort-Object)

    if ([string]::IsNullOrWhiteSpace($hub.properties.connectionProfile)) {
        throw "Hub created but connectionProfile is ABSENT -- the property was silently dropped; this is a Classic hub."
    }
    if ($hub.properties.connectionProfile -notmatch '^(?i)mqttv5$') {
        throw "connectionProfile came back '$($hub.properties.connectionProfile)', expected mqttv5."
    }
    $hub
}

# ------------------------------- 7. is it genuinely wired to an AEG namespace?
#  Bug bash section 4: the .device endpoint must resolve through an
#  'iothub-egns-<...>.ts.eventgrid.azure.net' alias. That alias is the only
#  customer-visible proof the hub is AEG-backed rather than merely flagged.
Step -Mutating -Critical "7. Resolve the .device endpoint and look for the AEG namespace alias" {
    # Prefer the read-only deviceHostName the service reports; fall back to the
    # conventional <hub>.device.azure-devices.net form.
    $deviceFqdn = $null
    if ($hubFinal -and $hubFinal.properties.deviceHostName) { $deviceFqdn = $hubFinal.properties.deviceHostName }
    if ([string]::IsNullOrWhiteSpace($deviceFqdn)) { $deviceFqdn = "$HubName.device.azure-devices.net" }
    Write-Host "  resolving $deviceFqdn"

    $chain = $null
    foreach ($tool in 'nslookup','dig','host') {
        if (Get-Command $tool -ErrorAction SilentlyContinue) {
            $chain = (& $tool $deviceFqdn 2>&1 | Out-String); break
        }
    }
    $script:Facts['deviceFqdnResolved'] = $deviceFqdn
    if (-not $chain) {
        # INCONCLUSIVE, not negative: no resolver tool here says nothing about the
        # hub. Warn and carry on rather than aborting -- gating steps 8/9 on the
        # probe's own blind spot would be wrong. The report records it as null so
        # the AEG question is visibly unanswered rather than silently passed.
        $script:Facts['aegAliasFound'] = $null
        $script:Facts['dnsChain']      = $null
        $script:StepNote = "no DNS tool on this host; the AEG alias was not verified"
        Write-Host "  INCONCLUSIVE: no DNS tool (nslookup/dig/host) on this host, so the AEG alias was NOT verified." -ForegroundColor Yellow
        Write-Host "  Re-check by hand: nslookup $deviceFqdn" -ForegroundColor Yellow
        return
    }
    Write-Host $chain
    $script:Facts['dnsChain']      = $chain
    $script:Facts['aegAliasFound'] = [bool]($chain -match 'ts\.eventgrid\.azure\.net')
    if ($chain -notmatch 'ts\.eventgrid\.azure\.net') {
        # Genuinely negative, and this step is -Critical: without the alias the
        # hub is not AEG-backed, so registering a device (8) or linking DPS (9)
        # against it would produce results that LOOK like AEG results and are not.
        throw "No '*.ts.eventgrid.azure.net' alias in the resolution chain -- the hub is NOT AEG-backed."
    }
    Write-Host "  AEG namespace alias found." -ForegroundColor Green
}

# ------------------------------------------ 8. service credentials + a X509 device
Step -Mutating "8. listkeys, build the .service connection string, register an X509 device" {
    $keysUrl = "$ArmEndpoint/subscriptions/$SubscriptionId/resourceGroups/$ResourceGroup/providers/Microsoft.Devices/IotHubs/$HubName/listkeys" +
               "?api-version=$HubApiVersion"
    $keys = Invoke-ArmRest -Method post -Url $keysUrl
    $owner = $keys.value | Where-Object { $_.keyName -eq 'iothubowner' } | Select-Object -First 1
    if (-not $owner) { throw "No 'iothubowner' policy returned by listkeys." }
    # NOTE: for AEG hubs the service endpoint is <hub>.service.azure-devices.net,
    # NOT the classic <hub>.azure-devices.net.
    Write-Host "  iothubowner key retrieved (not printed)."
    Write-Host "  service endpoint: $HubName.service.azure-devices.net"

    if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) { throw "openssl not found; cannot mint the device certificate." }
    $deviceId = "probe-device-01"
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ("mqtt5probe-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    # CN MUST equal the device id.
    & openssl req -x509 -newkey rsa:2048 -keyout "$dir/device-key.pem" -out "$dir/device-cert.pem" `
        -days 365 -nodes -subj "/CN=$deviceId" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "openssl req failed." }
    & openssl pkcs12 -export -in "$dir/device-cert.pem" -inkey "$dir/device-key.pem" -out "$dir/device.pfx" -passout "pass:probe" 2>$null
    $thumb = ((& openssl x509 -in "$dir/device-cert.pem" -noout -fingerprint -sha1) -replace '.*=','') -replace ':',''
    Write-Host "  deviceId=$deviceId thumbprint=$thumb"
    Write-Host "  cert material in $dir (device.pfx password 'probe')"

    # Registry is a data-plane call; do it with the azure-iot extension if present,
    # otherwise leave it to the service-side sample and say so.
    $ext = az extension list -o json 2>$null | ConvertFrom-Json | Where-Object { $_.name -eq 'azure-iot' }
    $script:Facts['azureIotExtensionVersion'] = if ($ext) { $ext.version } else { $null }
    if ($ext) {
        az iot hub device-identity create --hub-name $HubName --device-id $deviceId `
            --am x509_thumbprint --primary-thumbprint $thumb -o none
        if ($LASTEXITCODE -ne 0) { throw "device-identity create failed (the pinned extension may not understand an AEG hub)." }
        Write-Host "  device registered via az iot extension."
        $script:Facts['deviceRegistered'] = $true
    } else {
        Write-Host "  azure-iot extension absent -- register with RegistryManager.AddDeviceAsync from the service SDK." -ForegroundColor Yellow
        $script:Facts['deviceRegistered'] = $false
    }
    [pscustomobject]@{ DeviceId=$deviceId; Thumbprint=$thumb; CertDir=$dir }
}

# ------------------------------------- 9. BEYOND the documented flow: DPS
#  The documented flow connects the device straight to the hub, never using DPS. Our .NET
#  client has no public direct-hub connect (ConnectAsync is internal), so for .NET
#  e2e DPS must both accept an AEG hub and hand the profile back to the device.
#  The C client does have a direct-hub path (samples/authentication/direct-hub), so
#  C e2e could mirror that flow and skip all of this.
if (-not $SkipDps) {
    Step -Mutating "9. [beyond the documented flow] Create DPS, link the mqttv5 hub" {
        $dpsName = "mqtt5dps-$([guid]::NewGuid().ToString('N').Substring(0,12))"
        az iot dps create --name $dpsName --resource-group $ResourceGroup --location $Location -o none
        if ($LASTEXITCODE -ne 0) { throw "DPS create failed in $Location." }
        az iot dps linked-hub create --dps-name $dpsName --resource-group $ResourceGroup --hub-name $HubName -o none
        if ($LASTEXITCODE -ne 0) { throw "Linking an mqttv5 hub to DPS was REJECTED -- DPS may not accept AEG hubs yet." }
        $dps = az iot dps show --name $dpsName --resource-group $ResourceGroup -o json | ConvertFrom-Json
        Write-Host "  idScope     : $($dps.properties.idScope)"
        Write-Host "  linked hubs : $(($dps.properties.iotHubs | ForEach-Object { $_.name }) -join ', ')"
        $script:Facts['dpsLinkAccepted'] = $true
        $script:Facts['dpsIdScope']      = $dps.properties.idScope
        $script:Facts['dpsLinkedHubs']   = @($dps.properties.iotHubs | ForEach-Object { $_.name })
        Write-Host ""
        Write-Host "  STILL MANUAL: register a device through this DPS on data-plane api-version" -ForegroundColor Yellow
        Write-Host "  2026-11-02-preview and check the ASSIGNED payload for connectionProfile='mqttV5'." -ForegroundColor Yellow
        Write-Host "  If it is absent, AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE=mqttV5 stays mandatory in CI." -ForegroundColor Yellow
        $dps
    }
}

# ------------------------------------------------------------------- teardown
if (-not $WhatIfOnly -and -not $KeepResources -and $rgCreated) {
    Write-Host ""
    Write-Host "=== Teardown: deleting $ResourceGroup" -ForegroundColor Cyan
    az group delete --name $ResourceGroup --yes --no-wait -o none
}
elseif ($KeepResources) {
    Write-Host ""
    Write-Host "Resources kept. Delete with: az group delete --name $ResourceGroup --yes" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "=== Summary" -ForegroundColor Cyan
$script:Results | Format-Table -AutoSize -Wrap

# ----------------------------------------------------------------- the report
# This file is the deliverable. It contains no keys, no connection strings and no
# certificate material -- only resource names, states and the values the service
# echoed back.
$report = [ordered]@{
    schema          = 'aeg-mqtt5-probe/1'
    utc             = (Get-Date).ToUniversalTime().ToString('o')
    subscriptionId  = $SubscriptionId
    location        = $Location
    armEndpoint     = $ArmEndpoint
    hubApiVersion   = $HubApiVersion
    resourceGroup   = $ResourceGroup
    hubName         = $HubName
    requestedSku    = $Sku
    whatIfOnly      = [bool]$WhatIfOnly
    skipDps         = [bool]$SkipDps
    steps           = @($script:Results)
    facts           = $script:Facts
}
try {
    $report | ConvertTo-Json -Depth 30 | Set-Content -Path $ReportPath -Encoding utf8
    Write-Host ""
    Write-Host "Report written to $ReportPath" -ForegroundColor Green
    Write-Host "Transcript      $TranscriptPath" -ForegroundColor Green
    Write-Host "Send BOTH back. Neither contains a key or certificate." -ForegroundColor Green
}
catch { Write-Host "Could not write the report: $($_.Exception.Message)" -ForegroundColor Red }

try { Stop-Transcript | Out-Null } catch { }
