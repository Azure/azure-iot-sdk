#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Writes a device identity for an ESP-IDF build run in QEMU: the esp_secure_cert partition
# (espressif/esp_secure_cert_mgr's configure_esp_secure_cert.py) and, when the build uses the
# Digital Signature peripheral, an eFuse image holding the DS key. Requires an ESP-IDF environment
# (export.sh sourced) and QEMU, as eng/esp-idf-qemu.sh. Test identities only: the DS key is
# written without read protection.
#
#   eng/esp-idf-qemu-identity.sh <build-dir> <target> <cert.pem> <key.pem> <out-dir>
#
# Writes <out-dir>/esp_secure_cert.bin (for offset 0xd000) and, with DS, <out-dir>/qemu_efuse.bin
# (eFuse key block 3). DS takes an RSA key of at most 3072 bits.
set -euo pipefail

if [ $# -ne 5 ]; then
  echo "usage: $0 <build-dir> <target> <cert.pem> <key.pem> <out-dir>" >&2
  exit 2
fi
build=$(cd "$1" && pwd)
target=$2
cert=$(realpath "$3")
key=$(realpath "$4")
mkdir -p "$5"
out=$(cd "$5" && pwd)
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# Same release as the version the ESP-IDF component pins.
if ! command -v configure_esp_secure_cert.py > /dev/null; then
  python -m pip install --quiet esp-secure-cert-tool==2.6.1
fi

args=(--target_chip "$target" --secure_cert_type cust_flash_tlv --device-cert "$cert"
  --private-key "$key" --keep_ds_data_on_host --skip_flash)
cd "$out"
rm -rf esp_secure_cert_data qemu_efuse.bin

if grep -q '^#define CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL 1' "${build}/config/sdkconfig.h"; then
  if [ "$target" != esp32c3 ]; then
    echo "DS identities are written in QEMU for esp32c3 only" >&2
    exit 2
  fi
  bits=$(python -c 'import sys
from cryptography.hazmat.primitives.serialization import load_pem_private_key
print(load_pem_private_key(open(sys.argv[1], "rb").read(), None).key_size)' "$key")
  # A blank esp32c3 eFuse image at chip revision 0.3, as ESP-IDF's `idf.py qemu` writes it.
  python -c 'import sys; b = bytearray(1024); b[0x26] = 0x0c; open(sys.argv[1], "wb").write(b)' \
    qemu_efuse.bin
  port=$((5555 + RANDOM % 1000))
  QEMU_EFUSE="${out}/qemu_efuse.bin" bash "${here}/esp-idf-qemu.sh" "$build" "$target" \
    --download "$port" > qemu_download.log 2>&1 &
  pid=$!
  trap 'kill "$pid" 2> /dev/null || true; wait "$pid" 2> /dev/null || true' EXIT
  sleep 3
  # espefuse asks for BURN before writing the DS key into the eFuse image.
  echo BURN | configure_esp_secure_cert.py "${args[@]}" --port "socket://localhost:${port}" \
    --configure_ds --priv_key_algo RSA "$bits" --efuse_key_id 3 > tool.log 2>&1 \
    || { tail -n 40 tool.log; exit 1; }
  kill "$pid" 2> /dev/null || true
  wait "$pid" 2> /dev/null || true
  trap - EXIT
else
  configure_esp_secure_cert.py "${args[@]}" > tool.log 2>&1 || { tail -n 40 tool.log; exit 1; }
fi

mv esp_secure_cert_data/esp_secure_cert.bin esp_secure_cert.bin
ls -l esp_secure_cert.bin qemu_efuse.bin 2> /dev/null || true
