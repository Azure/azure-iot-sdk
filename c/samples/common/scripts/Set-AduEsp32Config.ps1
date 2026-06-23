#Requires -Version 7.0
<#
.SYNOPSIS
    Configures the ESP32 ADU sample with the device identity, DPS settings, and
    Wi-Fi credentials so it is ready to build and flash.

.DESCRIPTION
    Automates "Step 2" of samples/adu/esp32/README.md. Run this AFTER
    Initialize-AduSampleEnvironment.ps1, which generates the device certificate
    and exports the AZ_IOT_* environment variables this script consumes.

    It:
      1. Copies the device certificate + key into main/certs/ (embedded into the
         firmware), and the trusted CA if a real one was provided.
      2. Prompts for the Wi-Fi SSID and password (the only values not produced by
         the init script). Pass -WifiSsid / -WifiPassword to skip the prompts.
      3. Ensures an sdkconfig exists (generating it from sdkconfig.defaults via
         `idf.py set-target` if needed), then writes CONFIG_ADU_WIFI_SSID,
         CONFIG_ADU_WIFI_PASSWORD, CONFIG_ADU_DPS_ID_SCOPE and
         CONFIG_ADU_DPS_REGISTRATION_ID into it.

    The generated sdkconfig is git-ignored, so the Wi-Fi password and device
    settings are never committed.

.PARAMETER DeviceCert
    Path to the device certificate PEM. Defaults to AZ_IOT_CLIENT_CERT.

.PARAMETER DeviceKey
    Path to the device private key PEM. Defaults to AZ_IOT_CLIENT_KEY.

.PARAMETER TrustedCa
    Optional path to a CA chain PEM. Defaults to AZ_IOT_TRUSTED_CA. If it is not a
    real certificate the placeholder is kept and the adapter falls back to the
    ESP-IDF certificate bundle.

.PARAMETER IdScope
    DPS ID scope. Defaults to AZ_IOT_DPS_ID_SCOPE.

.PARAMETER RegistrationId
    DPS registration id (device id, must equal the certificate CN). Defaults to
    AZ_IOT_DPS_REGISTRATION_ID.

.PARAMETER WifiSsid
    Wi-Fi SSID. Prompted for if omitted.

.PARAMETER WifiPassword
    Wi-Fi password. Prompted for (securely) if omitted.

.PARAMETER IdfExportScript
    Path to the ESP-IDF export script. The PowerShell variant (export.ps1) next
    to it is dot-sourced to bring idf.py onto PATH when it is not already there.

.EXAMPLE
    ./Set-AduEsp32Config.ps1

.EXAMPLE
    ./Set-AduEsp32Config.ps1 -WifiSsid HomeNet -WifiPassword (Read-Host -AsSecureString)
#>
param(
    [string]$DeviceCert     = $env:AZ_IOT_CLIENT_CERT,
    [string]$DeviceKey      = $env:AZ_IOT_CLIENT_KEY,
    [string]$TrustedCa      = $env:AZ_IOT_TRUSTED_CA,
    [string]$IdScope        = $env:AZ_IOT_DPS_ID_SCOPE,
    [string]$RegistrationId = $env:AZ_IOT_DPS_REGISTRATION_ID,
    [string]$WifiSsid       = "",
    [securestring]$WifiPassword,
    [string]$Target          = "esp32",
    [string]$IdfExportScript = "C:\esp\v6.0\esp-idf\export.bat"
)

$ErrorActionPreference = "Stop"

# Locate the ESP32 project (../../adu/esp32 relative to this script).
$projectDir = (Resolve-Path (Join-Path $PSScriptRoot "..\..\adu\esp32")).Path
$certsDir   = Join-Path $projectDir "main\certs"
$sdkconfig  = Join-Path $projectDir "sdkconfig"

# --- validate required inputs ------------------------------------------------
if (-not $IdScope) {
    throw "DPS ID scope not set. Run Initialize-AduSampleEnvironment.ps1 first, or pass -IdScope."
}
if (-not $RegistrationId) {
    throw "DPS registration id not set. Run Initialize-AduSampleEnvironment.ps1 first, or pass -RegistrationId."
}
if (-not $DeviceCert -or -not (Test-Path $DeviceCert)) {
    throw "Device certificate not found: '$DeviceCert'. Run Initialize-AduSampleEnvironment.ps1 first, or pass -DeviceCert."
}
if (-not $DeviceKey -or -not (Test-Path $DeviceKey)) {
    throw "Device key not found: '$DeviceKey'. Run Initialize-AduSampleEnvironment.ps1 first, or pass -DeviceKey."
}

# --- 1. Embed the device identity --------------------------------------------
Write-Host "==> Copying device certificate + key into main/certs/"
Copy-Item $DeviceCert (Join-Path $certsDir "device_cert.pem") -Force
Copy-Item $DeviceKey  (Join-Path $certsDir "device_key.pem")  -Force

if ($TrustedCa -and (Test-Path $TrustedCa) -and
    (Select-String -Path $TrustedCa -Pattern "BEGIN CERTIFICATE" -Quiet)) {
    Write-Host "    using provided trusted CA: $TrustedCa"
    Copy-Item $TrustedCa (Join-Path $certsDir "trusted_ca.pem") -Force
} else {
    Write-Host "    no real trusted CA provided; keeping placeholder (ESP-IDF cert bundle fallback)"
}

# --- 2. Wi-Fi credentials (the only values not produced by the init script) ---
if (-not $WifiSsid) {
    $WifiSsid = Read-Host "Wi-Fi SSID"
}
if (-not $WifiSsid) {
    throw "A Wi-Fi SSID is required."
}
if (-not $WifiPassword) {
    $WifiPassword = Read-Host "Wi-Fi password" -AsSecureString
}
$wifiPasswordPlain = ConvertFrom-SecureString $WifiPassword -AsPlainText

# --- 3. Write the settings into sdkconfig ------------------------------------
if (-not (Test-Path $sdkconfig)) {
    Write-Host "==> No sdkconfig yet; generating it from sdkconfig.defaults (idf.py set-target $Target)"
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        $exportPs1 = [System.IO.Path]::ChangeExtension($IdfExportScript, ".ps1")
        if (-not (Test-Path $exportPs1)) {
            throw "ESP-IDF export script not found: $exportPs1 (derived from -IdfExportScript $IdfExportScript). Open an ESP-IDF PowerShell or pass -IdfExportScript."
        }
        Write-Host "    sourcing ESP-IDF environment: $exportPs1"
        . $exportPs1
    }
    idf.py -C $projectDir set-target $Target
    if ($LASTEXITCODE -ne 0) { throw "idf.py set-target failed." }
}

function Set-SdkConfigValue {
    param(
        [string]$Path,
        [string]$Key,
        [string]$Value   # raw string value; will be quoted + escaped
    )
    $escaped = $Value -replace '\\', '\\' -replace '"', '\"'
    $line    = "$Key=`"$escaped`""
    $content = Get-Content -Path $Path -Raw
    $pattern = "(?m)^(# )?$([regex]::Escape($Key))(=.*| is not set)$"
    if ($content -match $pattern) {
        $content = [regex]::Replace($content, $pattern, [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $line })
    } else {
        if ($content -and -not $content.EndsWith("`n")) { $content += "`n" }
        $content += "$line`n"
    }
    [System.IO.File]::WriteAllText($Path, $content, (New-Object System.Text.UTF8Encoding($false)))
}

Write-Host "==> Writing device + Wi-Fi settings into sdkconfig"
Set-SdkConfigValue -Path $sdkconfig -Key "CONFIG_ADU_WIFI_SSID"        -Value $WifiSsid
Set-SdkConfigValue -Path $sdkconfig -Key "CONFIG_ADU_WIFI_PASSWORD"    -Value $wifiPasswordPlain
Set-SdkConfigValue -Path $sdkconfig -Key "CONFIG_ADU_DPS_ID_SCOPE"     -Value $IdScope
Set-SdkConfigValue -Path $sdkconfig -Key "CONFIG_ADU_DPS_REGISTRATION_ID" -Value $RegistrationId

Write-Host ""
Write-Host "Configuration applied:"
Write-Host "  Wi-Fi SSID        : $WifiSsid"
Write-Host "  Wi-Fi password    : (hidden)"
Write-Host "  DPS ID scope      : $IdScope"
Write-Host "  DPS registration  : $RegistrationId"
Write-Host "  device_cert.pem   : <- $DeviceCert"
Write-Host "  device_key.pem    : <- $DeviceKey"
Write-Host ""
Write-Host "Next: flash and watch it connect:"
Write-Host "  idf.py -C `"$projectDir`" build flash monitor"
