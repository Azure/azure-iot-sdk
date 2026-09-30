# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
# Launches a Mosquitto MQTT 5 broker in Docker with:
#   - Plain TCP on port 1883
#   - TLS on port 8883
#
# Usage:
#   .\start_broker.ps1          # Start broker
#   .\start_broker.ps1 -Stop    # Stop broker

param(
  [switch]$Stop
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$CertsDir = Join-Path $ScriptDir "broker\certs"
$ContainerName = "az_mqtt_test_broker"

function Generate-Certs {
  if (Test-Path (Join-Path $CertsDir "server.crt")) {
    Write-Host "Certificates already exist, skipping generation."
    return
  }

  Write-Host "Generating self-signed TLS certificates..."
  New-Item -ItemType Directory -Force -Path $CertsDir | Out-Null

  # CA key and certificate
  openssl genrsa -out "$CertsDir\ca.key" 2048 2>$null
  openssl req -x509 -new -nodes `
    -key "$CertsDir\ca.key" `
    -sha256 -days 3650 `
    -subj "/CN=az_mqtt-test-ca" `
    -out "$CertsDir\ca.crt" 2>$null

  # Server key and CSR
  openssl genrsa -out "$CertsDir\server.key" 2048 2>$null
  openssl req -new `
    -key "$CertsDir\server.key" `
    -subj "/CN=localhost" `
    -out "$CertsDir\server.csr" 2>$null

  # SAN extension config
  $extConf = @"
[v3_req]
subjectAltName = DNS:localhost, IP:127.0.0.1
"@
  $extConf | Set-Content "$CertsDir\server_ext.cnf" -Encoding ASCII

  # Sign server certificate
  openssl x509 -req `
    -in "$CertsDir\server.csr" `
    -CA "$CertsDir\ca.crt" `
    -CAkey "$CertsDir\ca.key" `
    -CAcreateserial `
    -out "$CertsDir\server.crt" `
    -days 3650 -sha256 `
    -extfile "$CertsDir\server_ext.cnf" `
    -extensions v3_req 2>$null

  # Clean up
  Remove-Item -ErrorAction SilentlyContinue "$CertsDir\server.csr", "$CertsDir\server_ext.cnf", "$CertsDir\ca.srl"

  Write-Host "Certificates generated in $CertsDir"
}

function Start-Broker {
  # Stop any existing instance (ignore errors if container doesn't exist)
  $ErrorActionPreference = "SilentlyContinue"
  docker rm -f $ContainerName 2>&1 | Out-Null
  $ErrorActionPreference = "Stop"

  # Check if openssl is available for TLS
  $hasOpenSSL = $null -ne (Get-Command openssl -ErrorAction SilentlyContinue)

  if ($hasOpenSSL) {
    Generate-Certs
    $confFile = "mosquitto.conf"
  } else {
    Write-Host "OpenSSL not found - starting broker without TLS (plain TCP only)"
    $confFile = "mosquitto_notls.conf"
    # Create a no-TLS config if it doesn't exist
    $noTlsConf = Join-Path $ScriptDir "broker\mosquitto_notls.conf"
    if (-not (Test-Path $noTlsConf)) {
      @"
listener 1883
protocol mqtt
allow_anonymous true
"@ | Set-Content $noTlsConf -Encoding ASCII
    }
  }

  Write-Host "Starting Mosquitto MQTT 5 broker..."

  # Convert Windows paths to Docker-compatible paths
  $confPath = (Join-Path $ScriptDir "broker\$confFile") -replace '\\', '/'
  $certsPath = ($CertsDir -replace '\\', '/')

  $dockerArgs = @(
    "run", "-d",
    "--name", $ContainerName,
    "-p", "1883:1883"
  )

  if ($hasOpenSSL) {
    $dockerArgs += @("-p", "8883:8883")
    $dockerArgs += @("-v", "${confPath}:/mosquitto/config/mosquitto.conf:ro")
    $dockerArgs += @("-v", "${certsPath}:/mosquitto/certs:ro")
  } else {
    $dockerArgs += @("-v", "${confPath}:/mosquitto/config/mosquitto.conf:ro")
  }

  $dockerArgs += "eclipse-mosquitto:latest"
  & docker @dockerArgs

  # Wait for broker to be ready
  Write-Host -NoNewline "Waiting for broker"
  for ($i = 0; $i -lt 30; $i++) {
    $result = docker exec $ContainerName mosquitto_pub -h localhost -p 1883 -t '__health' -m 'ok' -V 5 2>&1
    if ($LASTEXITCODE -eq 0) {
      Write-Host " ready!"
      Write-Host "  Plain TCP: localhost:1883"
      if ($hasOpenSSL) {
        Write-Host "  TLS:       localhost:8883"
        Write-Host "  CA cert:   $CertsDir\ca.crt"
      } else {
        Write-Host "  TLS:       disabled (OpenSSL not found)"
      }
      return
    }
    Write-Host -NoNewline "."
    Start-Sleep -Seconds 1
  }

  Write-Host " TIMEOUT - broker may not be ready"
  docker logs $ContainerName
  throw "Broker did not start in time"
}

function Stop-Broker {
  Write-Host "Stopping Mosquitto broker..."
  $ErrorActionPreference = "SilentlyContinue"
  docker rm -f $ContainerName 2>&1 | Out-Null
  $ErrorActionPreference = "Stop"
  Write-Host "Stopped."
}

if ($Stop) {
  Stop-Broker
} else {
  Start-Broker
}
