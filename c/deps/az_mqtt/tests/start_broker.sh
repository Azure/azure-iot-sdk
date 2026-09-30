#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
# Launches a Mosquitto MQTT 5 broker in Docker with:
#   - Plain TCP on port 1883
#   - TLS on port 8883
#
# Usage:
#   ./start_broker.sh          # Start broker
#   ./start_broker.sh stop     # Stop broker

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERTS_DIR="${SCRIPT_DIR}/broker/certs"
CONTAINER_NAME="az_mqtt_test_broker"

generate_certs() {
  if [ -f "${CERTS_DIR}/server.crt" ]; then
    echo "Certificates already exist, skipping generation."
    return
  fi

  echo "Generating self-signed TLS certificates..."
  mkdir -p "${CERTS_DIR}"

  # CA key and certificate
  openssl genrsa -out "${CERTS_DIR}/ca.key" 2048 2>/dev/null
  openssl req -x509 -new -nodes \
    -key "${CERTS_DIR}/ca.key" \
    -sha256 -days 3650 \
    -subj "/CN=az_mqtt-test-ca" \
    -out "${CERTS_DIR}/ca.crt" 2>/dev/null

  # Server key and CSR
  openssl genrsa -out "${CERTS_DIR}/server.key" 2048 2>/dev/null
  openssl req -new \
    -key "${CERTS_DIR}/server.key" \
    -subj "/CN=localhost" \
    -out "${CERTS_DIR}/server.csr" 2>/dev/null

  # Sign server certificate with CA (include SAN for localhost + 127.0.0.1)
  cat > "${CERTS_DIR}/server_ext.cnf" <<EOF
[v3_req]
subjectAltName = DNS:localhost, IP:127.0.0.1
EOF

  openssl x509 -req \
    -in "${CERTS_DIR}/server.csr" \
    -CA "${CERTS_DIR}/ca.crt" \
    -CAkey "${CERTS_DIR}/ca.key" \
    -CAcreateserial \
    -out "${CERTS_DIR}/server.crt" \
    -days 3650 -sha256 \
    -extfile "${CERTS_DIR}/server_ext.cnf" \
    -extensions v3_req 2>/dev/null

  # Clean up intermediate files
  rm -f "${CERTS_DIR}/server.csr" "${CERTS_DIR}/server_ext.cnf" "${CERTS_DIR}/ca.srl"

  echo "Certificates generated in ${CERTS_DIR}"
}

start_broker() {
  # Stop any existing instance
  docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true

  generate_certs

  echo "Starting Mosquitto MQTT 5 broker..."
  docker run -d \
    --name "${CONTAINER_NAME}" \
    -p 1883:1883 \
    -p 8883:8883 \
    -v "${SCRIPT_DIR}/broker/mosquitto.conf:/mosquitto/config/mosquitto.conf:ro" \
    -v "${CERTS_DIR}:/mosquitto/certs:ro" \
    eclipse-mosquitto:latest

  # Wait for broker to be ready
  echo -n "Waiting for broker"
  for i in $(seq 1 30); do
    if docker exec "${CONTAINER_NAME}" mosquitto_pub -h localhost -p 1883 -t '__health' -m 'ok' -V 5 2>/dev/null; then
      echo " ready!"
      echo "  Plain TCP: localhost:1883"
      echo "  TLS:       localhost:8883"
      echo "  CA cert:   ${CERTS_DIR}/ca.crt"
      return 0
    fi
    echo -n "."
    sleep 1
  done

  echo " TIMEOUT - broker may not be ready"
  docker logs "${CONTAINER_NAME}"
  return 1
}

stop_broker() {
  echo "Stopping Mosquitto broker..."
  docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true
  echo "Stopped."
}

case "${1:-start}" in
  start) start_broker ;;
  stop)  stop_broker ;;
  *)     echo "Usage: $0 {start|stop}" ; exit 1 ;;
esac
