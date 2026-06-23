#Requires -Version 7.2
<#
.SYNOPSIS
    Provisions a local IoT-Hub-Next mock environment for manual telemetry testing.

.DESCRIPTION
    1. Checks prerequisites (Docker, PowerShell 7.2+).
    2. Generates a full X.509 PKI (CA → server → service-worker → device) using
       .NET crypto only (no OpenSSL dependency).
    3. Writes mosquitto config + docker-compose.yml.
    4. Launches the MQTT v5 broker in Docker.

    If the environment is already running, informs the user and exits.

.PARAMETER DeviceId
    Device identity used as the certificate CN and MQTT client-id.
    Default: "test-device-01"

.PARAMETER BrokerPort
    TLS listener port exposed to the host. Default: 8883

.EXAMPLE
    .\New-TestEnv.ps1
    .\New-TestEnv.ps1 -DeviceId "my-device" -BrokerPort 18883
#>
[CmdletBinding()]
param(
    [string]$DeviceId = "test-device-01",
    [int]$BrokerPort = 8883,
    [string]$HubNextRepo = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = $PSScriptRoot
$CertsDir  = Join-Path $ScriptDir ".certs"
$ComposeDir = Join-Path $ScriptDir ".env-hub-next"

# Resolve az-iot-hub-next repo location (default: sibling directory)
if (-not $HubNextRepo) {
    $RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
    $HubNextRepo = Join-Path (Split-Path $RepoRoot -Parent) "az-iot-hub-next"
}
if (-not (Test-Path (Join-Path $HubNextRepo "service\main.c"))) {
    throw "az-iot-hub-next repo not found at '$HubNextRepo'. Pass -HubNextRepo <path> or clone it as a sibling."
}

# ─────────────────────────────────────────────────────────────────────────────
# Prerequisites
# ─────────────────────────────────────────────────────────────────────────────

function Assert-Prerequisites {
    if ($PSVersionTable.PSVersion.Major -lt 7 -or
        ($PSVersionTable.PSVersion.Major -eq 7 -and $PSVersionTable.PSVersion.Minor -lt 2)) {
        throw "PowerShell 7.2+ required (for .NET 6 crypto APIs). Current: $($PSVersionTable.PSVersion)"
    }

    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw "Docker not found in PATH. Install Docker Desktop or Docker Engine."
    }

    # Check if daemon is responsive; if not, attempt to start Docker Desktop.
    $null = docker info 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[prereq] Docker daemon not running. Attempting to start Docker Desktop..."

        # Locate Docker Desktop executable
        $ddCmd = Get-Command "Docker Desktop" -ErrorAction SilentlyContinue
        $ddPath = if ($ddCmd) { $ddCmd.Source } else { "$env:ProgramFiles\Docker\Docker\Docker Desktop.exe" }
        if (-not (Test-Path $ddPath)) {
            throw "Docker daemon is not running and Docker Desktop was not found at '$ddPath'. Start Docker manually."
        }

        Start-Process -FilePath $ddPath
        Write-Host "[prereq] Waiting for Docker daemon..." -NoNewline

        $ready = $false
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Seconds 2
            $null = docker info 2>&1
            if ($LASTEXITCODE -eq 0) { $ready = $true; break }
            Write-Host "." -NoNewline
        }
        Write-Host ""
        if (-not $ready) {
            throw "Docker daemon did not become ready within 120 seconds. Start Docker Desktop manually."
        }
        Write-Host "[prereq] Docker is running." -ForegroundColor Green
    }
}

# ─────────────────────────────────────────────────────────────────────────────
# Check if already running
# ─────────────────────────────────────────────────────────────────────────────

function Test-EnvRunning {
    if (-not (Test-Path (Join-Path $ComposeDir "docker-compose.yml"))) {
        return $false
    }
    Push-Location $ComposeDir
    try {
        $ps = docker compose ps --format json 2>$null | ConvertFrom-Json
        if ($ps -and $ps.Count -gt 0) {
            return $true
        }
    } catch { }
    finally { Pop-Location }
    return $false
}

# ─────────────────────────────────────────────────────────────────────────────
# Certificate generation (native .NET — no OpenSSL)
# ─────────────────────────────────────────────────────────────────────────────

function New-CertificateAuthority {
    param([string]$OutDir)

    $key = [System.Security.Cryptography.RSA]::Create(2048)
    $dn  = [System.Security.Cryptography.X509Certificates.X500DistinguishedName]::new("CN=Azure IoT Test CA")

    $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        $dn, $key, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)

    # Basic Constraints: CA=true
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $false, 0, $true))

    # Key Usage: KeyCertSign, CRLSign
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign -bor
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::CrlSign, $true))

    $notBefore = [DateTimeOffset]::UtcNow.AddMinutes(-5)
    $notAfter  = [DateTimeOffset]::UtcNow.AddDays(365)

    $cert = $req.CreateSelfSigned($notBefore, $notAfter)

    # Export
    $certPem = $cert.ExportCertificatePem()
    $keyPem  = $key.ExportRSAPrivateKeyPem()

    Set-Content -Path (Join-Path $OutDir "ca.crt") -Value $certPem -NoNewline
    Set-Content -Path (Join-Path $OutDir "ca.key") -Value $keyPem  -NoNewline

    return $cert
}

function New-SignedCertificate {
    param(
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Issuer,
        [string]$CommonName,
        [string[]]$SubjectAlternativeNames,
        [string]$OutDir,
        [string]$FilePrefix
    )

    $key = [System.Security.Cryptography.RSA]::Create(2048)
    $dn  = [System.Security.Cryptography.X509Certificates.X500DistinguishedName]::new("CN=$CommonName")

    $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
        $dn, $key, [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)

    # Key Usage: DigitalSignature, KeyEncipherment
    $req.CertificateExtensions.Add(
        [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor
            [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyEncipherment, $true))

    # SAN
    if ($SubjectAlternativeNames -and $SubjectAlternativeNames.Count -gt 0) {
        $sanBuilder = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
        foreach ($san in $SubjectAlternativeNames) {
            if ($san -match '^\d+\.\d+\.\d+\.\d+$') {
                $sanBuilder.AddIpAddress([System.Net.IPAddress]::Parse($san))
            } else {
                $sanBuilder.AddDnsName($san)
            }
        }
        $req.CertificateExtensions.Add($sanBuilder.Build())
    }

    $serial = [byte[]]::new(16)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($serial)
    $serial[0] = $serial[0] -band 0x7F  # Ensure positive

    $notBefore = [DateTimeOffset]::UtcNow.AddMinutes(-5)
    $notAfter  = [DateTimeOffset]::UtcNow.AddDays(365)

    $issuerKey = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($Issuer)

    $gen = [System.Security.Cryptography.X509Certificates.X509SignatureGenerator]::CreateForRSA(
        $issuerKey, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)

    $cert = $req.Create($Issuer.SubjectName, $gen, $notBefore, $notAfter, $serial)

    # Export
    $certPem = $cert.ExportCertificatePem()
    $keyPem  = $key.ExportRSAPrivateKeyPem()

    Set-Content -Path (Join-Path $OutDir "$FilePrefix.crt") -Value $certPem -NoNewline
    Set-Content -Path (Join-Path $OutDir "$FilePrefix.key") -Value $keyPem  -NoNewline

    return $cert
}

function New-Pki {
    param([string]$OutDir, [string]$DevId)

    if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

    Write-Host "[certs] Generating CA..."
    $ca = New-CertificateAuthority -OutDir $OutDir

    Write-Host "[certs] Generating server cert (SAN: localhost, 127.0.0.1)..."
    New-SignedCertificate -Issuer $ca -CommonName "mqtt-broker" `
        -SubjectAlternativeNames @("localhost", "127.0.0.1") `
        -OutDir $OutDir -FilePrefix "server" | Out-Null

    Write-Host "[certs] Generating service-worker cert..."
    New-SignedCertificate -Issuer $ca -CommonName "service-worker" `
        -SubjectAlternativeNames @() `
        -OutDir $OutDir -FilePrefix "service-worker" | Out-Null

    Write-Host "[certs] Generating device cert (CN=$DevId)..."
    New-SignedCertificate -Issuer $ca -CommonName $DevId `
        -SubjectAlternativeNames @() `
        -OutDir $OutDir -FilePrefix $DevId | Out-Null

    Write-Host "[certs] PKI generated in: $OutDir"
}

# ─────────────────────────────────────────────────────────────────────────────
# Docker Compose + Mosquitto config
# ─────────────────────────────────────────────────────────────────────────────

function New-BrokerConfig {
    param([string]$Dir, [string]$CertsRel, [int]$Port)

    if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Path $Dir -Force | Out-Null }

    # mosquitto.conf
    $mqttConf = @"
# TLS listener (mTLS — device CN = username)
listener 8883
protocol mqtt
cafile /mosquitto/certs/ca.crt
certfile /mosquitto/certs/server.crt
keyfile /mosquitto/certs/server.key
require_certificate true
use_identity_as_username true

# Access control
acl_file /mosquitto/config/acl.conf

# Session persistence
persistence true
persistence_location /mosquitto/data/
autosave_interval 60

# Logging
log_type all
log_timestamp true
log_timestamp_format %Y-%m-%dT%H:%M:%S
connection_messages true

# Protocol
max_inflight_messages 20
max_keepalive 65535
"@
    Set-Content -Path (Join-Path $Dir "mosquitto.conf") -Value $mqttConf -NoNewline

    # acl.conf
    $aclConf = @"
# Device permissions (per-device via %u = cert CN)
pattern write ih/%u/srv/#
pattern read ih/%u/dev/#

# Allow any authenticated client to publish control commands
pattern write svc/cmd/#

# Service worker
user service-worker
topic read ih/+/srv/#
topic write ih/+/dev/#
topic readwrite health/#
topic readwrite svc/cmd/#
topic read `$SYS/#
"@
    Set-Content -Path (Join-Path $Dir "acl.conf") -Value $aclConf -NoNewline

    # docker-compose.yml — certs path relative to compose dir
    $certsRelPosix = $CertsRel -replace '\\','/'
    $compose = @"
# Auto-generated by New-TestEnv.ps1 — do not edit manually.
services:
  mosquitto:
    image: eclipse-mosquitto:2
    ports:
      - "${Port}:8883"
    volumes:
      - ./mosquitto.conf:/mosquitto/config/mosquitto.conf:ro
      - ./acl.conf:/mosquitto/config/acl.conf:ro
      - ${certsRelPosix}:/mosquitto/certs:ro
      - mosquitto-data:/mosquitto/data
    healthcheck:
      test: ["CMD-SHELL", "mosquitto_pub --cafile /mosquitto/certs/ca.crt --cert /mosquitto/certs/service-worker.crt --key /mosquitto/certs/service-worker.key -h localhost -p 8883 -V mqttv5 -t health/ping -m ok -q 0"]
      interval: 5s
      timeout: 5s
      retries: 5
      start_period: 3s

  service:
    build:
      context: ./service-build
    depends_on:
      mosquitto:
        condition: service_healthy
    volumes:
      - ${certsRelPosix}:/certs:ro
    environment:
      - MQTT_HOST=mosquitto
      - MQTT_PORT=8883
      - CA_CERT_PATH=/certs/ca.crt
      - CLIENT_CERT_PATH=/certs/service-worker.crt
      - CLIENT_KEY_PATH=/certs/service-worker.key
    restart: unless-stopped

volumes:
  mosquitto-data:
"@
    Set-Content -Path (Join-Path $Dir "docker-compose.yml") -Value $compose -NoNewline

    Write-Host "[broker] Config written to: $Dir"
}

# ─────────────────────────────────────────────────────────────────────────────
# Service build staging (pre-stage deps so Docker build needs no network)
# ─────────────────────────────────────────────────────────────────────────────

function New-ServiceBuildContext {
    param([string]$ComposeDir, [string]$ServiceSrcDir)

    $stageDir = Join-Path $ComposeDir "service-build"
    if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

    # Copy service source
    Copy-Item (Join-Path $ServiceSrcDir "main.c") $stageDir
    Copy-Item (Join-Path $ServiceSrcDir "CMakeLists.txt") $stageDir

    # Resolve az_mqtt dependency (check sibling first, then clone)
    $repoParent = Split-Path (Split-Path $ServiceSrcDir -Parent) -Parent
    $azMqttLocal = Join-Path $repoParent "az_mqtt"
    if (-not (Test-Path (Join-Path $azMqttLocal "az_mqtt5"))) {
        # Not found as sibling of az-iot-hub-next — try sibling of azure-iot-sdk
        $sdkParent = Split-Path (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path -Parent
        $azMqttLocal = Join-Path $sdkParent "az_mqtt"
    }

    $depsDir = Join-Path $stageDir "deps"
    New-Item -ItemType Directory -Path $depsDir -Force | Out-Null

    if (Test-Path (Join-Path $azMqttLocal "az_mqtt5")) {
        Write-Host "[service] Using local az_mqtt: $azMqttLocal"
        # Copy the full az_mqtt tree (includes az_mqtt5/ and its deps/)
        Copy-Item $azMqttLocal (Join-Path $depsDir "az_mqtt") -Recurse
    } else {
        Write-Host "[service] Cloning az_mqtt from GitHub (with submodules)..."
        git clone --depth 1 --recurse-submodules --shallow-submodules --branch main "https://github.com/ewertons/az_mqtt.git" (Join-Path $depsDir "az_mqtt") 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) { throw "Failed to clone az_mqtt. Ensure 'git' works and the repo is accessible." }
    }

    # Generate Dockerfile (multi-stage, offline build)
    $dockerfile = @"
# Auto-generated by New-TestEnv.ps1
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src/build

# Point CMake at the pre-staged az_mqtt source (contains az_mqtt5/ subdir) so no git clone is needed.
# Disable vcpkg auto-fetch (azure-sdk-for-c doesn't need it for this minimal build).
ENV AZURE_SDK_DISABLE_AUTO_VCPKG=1
RUN cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_SOURCE_DIR_AZ_MQTT=/src/deps/az_mqtt \
    -DAZ_MQTT5_BUILD_SAMPLES=OFF \
    && cmake --build . -j`$(nproc)

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/build/mock_hub_service /usr/local/bin/mock_hub_service
ENTRYPOINT ["mock_hub_service"]
"@
    Set-Content -Path (Join-Path $stageDir "Dockerfile") -Value $dockerfile -NoNewline
    Write-Host "[service] Build context staged in: $stageDir"
}

# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

Write-Host "=== IoT Hub-Next Mock Environment Setup ===" -ForegroundColor Cyan

Assert-Prerequisites

if (Test-EnvRunning) {
    Write-Host "Environment is already running. Use Remove-TestEnv.ps1 to tear down first." -ForegroundColor Yellow
    exit 0
}

New-Pki -OutDir $CertsDir -DevId $DeviceId

# Relative path from compose dir to certs dir
$certsRel = [System.IO.Path]::GetRelativePath($ComposeDir, $CertsDir)
New-BrokerConfig -Dir $ComposeDir -CertsRel $certsRel -Port $BrokerPort

$serviceSrcDir = Join-Path $HubNextRepo "service"
New-ServiceBuildContext -ComposeDir $ComposeDir -ServiceSrcDir $serviceSrcDir

Write-Host "[docker] Starting broker + service worker..."
Push-Location $ComposeDir
try {
    docker compose up -d --build 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "docker compose up failed" }

    # Wait for healthy
    Write-Host "[docker] Waiting for broker health check..." -NoNewline
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 2
        $health = docker compose ps --format json 2>$null | ConvertFrom-Json
        if ($health -and $health.Health -eq "healthy") {
            Write-Host " OK" -ForegroundColor Green
            break
        }
        Write-Host "." -NoNewline
    }
    if ($i -eq 30) {
        Write-Warning "Broker did not become healthy in 60s. Check: docker compose logs"
    }
} finally { Pop-Location }

Write-Host ""
Write-Host "Environment ready." -ForegroundColor Green
Write-Host "  Broker:   localhost:$BrokerPort (MQTT v5, mTLS)"
Write-Host "  Service:  mock_hub_service (telemetry + session tracking)"
Write-Host "  Device:   $DeviceId"
Write-Host "  CA cert:  $CertsDir\ca.crt"
Write-Host "  Dev cert: $CertsDir\$DeviceId.crt"
Write-Host "  Dev key:  $CertsDir\$DeviceId.key"
Write-Host ""
Write-Host "  View service logs: .\Read-ServiceLogs.ps1 -Follow"
Write-Host ""
Write-Host "Run .\New-TestClient.ps1 to build and configure the sample." -ForegroundColor Cyan
