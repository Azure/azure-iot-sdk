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
# times out). Runs apt-get through sudo unless already root.

set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: ${0##*/} <package>..." 1>&2; exit 1; }

as_root=()
[ "$(id -u)" -eq 0 ] || as_root=(sudo)

# retry <seconds> <command>...: run with a time limit, up to 3 attempts.
retry() {
    local limit="$1" attempt
    shift
    for attempt in 1 2 3; do
        if timeout "${limit}" "$@"; then
            return 0
        fi
        echo "attempt ${attempt} failed: $*" 1>&2
        if [ "${attempt}" -lt 3 ]; then sleep 15; fi
    done
    return 1
}

retry 300 "${as_root[@]}" apt-get update -q -o Acquire::Retries=3
retry 900 "${as_root[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y -q -o Acquire::Retries=3 "$@"
