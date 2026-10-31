#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Boots an ESP-IDF build in Espressif's QEMU with OpenCores Ethernet (user networking) and waits
# for a line of its log. Requires an ESP-IDF environment (export.sh sourced) with qemu-xtensa or
# qemu-riscv32 installed.
#
#   eng/esp-idf-qemu.sh <build-dir> <target> <regex> [seconds]
#
# target: esp32, esp32s3 or esp32c3. Exits 0 once a log line matches <regex> (grep -E), 1 when
# QEMU exits or <seconds> (default 120) pass first. The log is <build-dir>/qemu.log.
#
# Each run writes a fresh flash image from the build, so NVS and OTA state do not persist.
# QEMU_FLASH_EXTRA="<offset> <file> ..." also writes those files (relative to the current
# directory) into it (e.g. the device identity:
# "0xd000 esp_secure_cert.bin").
set -euo pipefail

if [ $# -lt 3 ]; then
  echo "usage: $0 <build-dir> <target> <regex> [seconds]" >&2
  exit 2
fi
build=$(cd "$1" && pwd)
target=$2
expect=$3
seconds=${4:-120}

case "$target" in
  esp32) qemu=(qemu-system-xtensa -M esp32 -m 4M) ;;
  esp32s3) qemu=(qemu-system-xtensa -M esp32s3) ;;
  esp32c3) qemu=(qemu-system-riscv32 -M esp32c3) ;;
  *)
    echo "no QEMU for target: $target" >&2
    exit 2
    ;;
esac

# export.sh puts QEMU, an optional tool, on PATH only in some installs.
if ! command -v "${qemu[0]}" > /dev/null; then
  found=$(find "${IDF_TOOLS_PATH:-${HOME}/.espressif}/tools" -name "${qemu[0]}" -type f 2> /dev/null | head -n 1)
  if [ -z "$found" ]; then
    echo "${qemu[0]} not found (idf_tools.py install qemu-xtensa qemu-riscv32)" >&2
    exit 2
  fi
  PATH="$(dirname "$found"):${PATH}"
fi

flash="${build}/qemu_flash.bin"
log="${build}/qemu.log"
# QEMU_FLASH_EXTRA's files are relative to the caller; esptool runs in the build directory.
extra=()
read -r -a pairs <<< "${QEMU_FLASH_EXTRA:-}"
if [ $((${#pairs[@]} % 2)) -ne 0 ]; then
  echo "QEMU_FLASH_EXTRA: expected <offset> <file> pairs" >&2
  exit 2
fi
for ((i = 0; i < ${#pairs[@]}; i += 2)); do
  file=${pairs[i + 1]}
  if [ ! -f "$file" ]; then
    echo "QEMU_FLASH_EXTRA: no such file: $file" >&2
    exit 2
  fi
  extra+=("${pairs[i]}" "$(cd "$(dirname "$file")" && pwd)/$(basename "$file")")
done
(cd "$build" && python -m esptool --chip "$target" merge-bin --pad-to-size 4MB -o "$flash" \
  @flash_args "${extra[@]}" > /dev/null)

"${qemu[@]}" -nographic -drive "file=${flash},if=mtd,format=raw" -nic user,model=open_eth \
  > "$log" 2>&1 &
pid=$!
trap 'kill "$pid" 2> /dev/null || true; wait "$pid" 2> /dev/null || true' EXIT

status=1
for _ in $(seq 0 "$seconds"); do
  if grep -aqE -e "$expect" "$log"; then
    status=0
    break
  fi
  kill -0 "$pid" 2> /dev/null || break
  sleep 1
done
# A line written as QEMU exited, or in the last second.
if [ "$status" -ne 0 ] && grep -aqE -e "$expect" "$log"; then
  status=0
fi

grep -aE -e "$expect" "$log" | head -n 5 || true
if [ "$status" -ne 0 ]; then
  tail -n 60 "$log"
  echo "no line matching '${expect}' within ${seconds} s" >&2
fi
exit "$status"
