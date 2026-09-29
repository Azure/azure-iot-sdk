#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Runs clang-tidy (config: c/.clang-tidy) on every first-party translation unit (src, adapters,
# samples) in a configured build tree. Exits non-zero on any finding.
#
#   eng/clang-tidy.sh <build-dir> [clang-tidy executable]
#
# The build tree only needs to be configured, with CMAKE_EXPORT_COMPILE_COMMANDS=ON.

set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: ${0##*/} <build-dir> [clang-tidy]" 1>&2; exit 1; }

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(cd "$1" && pwd)"
tidy="${2:-clang-tidy}"
db="${build_dir}/compile_commands.json"

[ -f "${db}" ] || { echo "${db} not found" 1>&2; exit 1; }
command -v "${tidy}" >/dev/null || { echo "${tidy} not found" 1>&2; exit 1; }

mapfile -t files < <(jq -r '.[].file' "${db}" \
    | grep -E "^${root_dir}/(src|adapters|samples)/" | sort -u)
[ "${#files[@]}" -gt 0 ] || { echo "no first-party sources in ${db}" 1>&2; exit 1; }

echo "$("${tidy}" --version | grep -m1 -i version)"
echo "clang-tidy: ${#files[@]} translation units"

# One process per file. The "N warnings generated" lines count diagnostics outside the header
# filter; drop them. pipefail keeps xargs' exit status (123 when any file has a finding).
printf '%s\0' "${files[@]}" \
    | xargs -0 -P "$(nproc)" -n 1 "${tidy}" -p "${build_dir}" --quiet 2>&1 \
    | { grep -vE '^[0-9]+ warnings? generated\.$' || true; }
