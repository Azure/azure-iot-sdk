#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Install Debian/Ubuntu packages for CI, failing fast instead of hanging.
#
#   eng/apt-install.sh <package>...
#
# Each apt-get call gets a time limit and up to 3 attempts: Ubuntu mirrors fail
# both ways (errors during a mirror sync, and fetches that stall until the job
# times out). All attempts together stop after 10 minutes, well inside the
# shortest caller's job timeout (30 minutes). Runs apt-get through sudo unless
# already root.

set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: ${0##*/} <package>..." 1>&2; exit 1; }

as_root=()
[ "$(id -u)" -eq 0 ] || as_root=(sudo)

deadline=$((SECONDS + 600))

# retry <seconds> <command>...: run with a time limit, up to 3 attempts, all before the deadline.
retry() {
    local limit="$1" attempt remaining
    shift
    for attempt in 1 2 3; do
        remaining=$((deadline - SECONDS))
        if [ "${remaining}" -le 0 ]; then
            break
        fi
        if timeout "$((remaining < limit ? remaining : limit))" "$@"; then
            return 0
        fi
        echo "attempt ${attempt} failed: $*" 1>&2
        if [ "${attempt}" -lt 3 ]; then sleep 15; fi
    done
    echo "giving up: $*" 1>&2
    return 1
}

retry 120 "${as_root[@]}" apt-get update -q -o Acquire::Retries=3
retry 300 "${as_root[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y -q -o Acquire::Retries=3 "$@"
