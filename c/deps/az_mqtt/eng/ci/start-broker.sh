#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Start a local Mosquitto for the e2e tests: plain MQTT on 1883, TLS on 8883.
#
#   eng/ci/start-broker.sh [work-dir]
#
# Mints a CA and a server certificate (SAN localhost, 127.0.0.1) and copies the
# CA to tests/broker/certs/ca.crt, where the e2e tests look for it.
# Needs openssl, mosquitto and mosquitto_pub on PATH.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="${1:-${root}/build/broker}"
mkdir -p "${work}"

# Windows (Git Bash): keep "/CN=..." from being rewritten as a path, and give
# mosquitto.exe native paths.
export MSYS_NO_PATHCONV=1
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

w="$(native "${work}")" # every path handed to openssl / mosquitto
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 30 -subj "/CN=az-mqtt-ci-ca" \
  -keyout "${w}/ca.key" -out "${w}/ca.crt" 2>&1 | grep -v '^[.+*]*$' || true
openssl req -newkey rsa:2048 -nodes -sha256 -subj "/CN=localhost" \
  -keyout "${w}/server.key" -out "${w}/server.csr" 2>&1 | grep -v '^[.+*]*$' || true
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > "${work}/san.cnf"
openssl x509 -req -in "${w}/server.csr" -CA "${w}/ca.crt" -CAkey "${w}/ca.key" \
  -CAcreateserial -days 30 -sha256 -extfile "${w}/san.cnf" -out "${w}/server.crt"
[ -s "${work}/server.crt" ] || { echo "certificate generation failed" >&2; exit 1; }

mkdir -p "${root}/tests/broker/certs"
cp "${work}/ca.crt" "${root}/tests/broker/certs/ca.crt"

cat > "${work}/mosquitto.conf" <<CONF
per_listener_settings false
allow_anonymous true
listener 1883 127.0.0.1
listener 8883 127.0.0.1
cafile ${w}/ca.crt
certfile ${w}/server.crt
keyfile ${w}/server.key
CONF

# A packaged broker may already own 1883.
if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet mosquitto 2>/dev/null; then
  sudo systemctl stop mosquitto
fi

nohup mosquitto -c "${w}/mosquitto.conf" > "${work}/mosquitto.log" 2>&1 &

for _ in $(seq 1 30); do
  if mosquitto_pub -h 127.0.0.1 -p 1883 -t ci/ready -m ok 2>/dev/null \
    && mosquitto_pub -h localhost -p 8883 --cafile "${w}/ca.crt" -t ci/ready -m ok 2>/dev/null; then
    echo "Mosquitto ready (1883, 8883)"
    exit 0
  fi
  sleep 1
done
echo "Mosquitto did not start:" >&2
cat "${work}/mosquitto.log" >&2
exit 1
