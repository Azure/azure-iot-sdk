#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Fails when the library reaches for a C construct that this SDK has an
# azure-sdk-for-c-shaped replacement for. The rules and the reasoning behind
# them live in c/docs/eng/coding-conventions.md; this script is only the part
# that makes them stick.
#
# Scope is c/src (core + features) on purpose. Adapters are the boundary where
# a third-party library, libc or OpenSSL is unavoidable, and tests and samples
# are application code.
#
# Escape hatch: a file may opt out of one symbol by carrying a comment
#
#     az-iot-allow: <symbol> -- <why this file genuinely cannot use the SDK helper>
#
# The waiver is per file and per symbol, and it is deliberately noisy to write,
# because every one of them is a decision a reviewer should see.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scan_dir="${root_dir}/src"

# symbol|what to use instead
banned=(
    "sprintf|az_iot_span_writer"
    "snprintf|az_iot_span_writer"
    "vsprintf|az_iot_span_writer"
    "vsnprintf|az_iot_span_writer"
    "strcpy|az_iot_span_writer_append_str"
    "strncpy|az_iot_span_writer_append_str"
    "strcat|az_iot_span_writer_append_str"
    "strncat|az_iot_span_writer_append_str"
    "strtok|az_span_find and az_span_slice"
    "strdup|a caller-provided buffer"
    "printf|AZ_IOT_LOG_* / AZ_IOT_LOG_*F"
    "fprintf|AZ_IOT_LOG_* / AZ_IOT_LOG_*F"
    "puts|AZ_IOT_LOG_*"
    "fputs|AZ_IOT_LOG_*"
    "malloc|a caller-provided buffer or in-struct storage"
    "calloc|a caller-provided buffer or in-struct storage"
    "realloc|a caller-provided buffer or in-struct storage"
    "free|a caller-provided buffer or in-struct storage"
)

violations=0

for entry in "${banned[@]}"; do
    symbol="${entry%%|*}"
    instead="${entry#*|}"

    while IFS= read -r hit; do
        [ -n "${hit}" ] || continue
        file="${hit%%:*}"

        # A file that documents why it needs this symbol is exempt from it.
        # POSIX character classes rather than \b: the boundary escape is a GNU
        # extension, and where it is unsupported a waiver silently stops
        # matching, which turns this check into a wall of false positives.
        if grep -qE "az-iot-allow:[[:space:]]*${symbol}([^[:alnum:]_]|\$)" "${file}"; then
            continue
        fi

        if [ "${violations}" -eq 0 ]; then
            echo "Banned constructs found in c/src:"
            echo
        fi
        violations=$((violations + 1))
        echo "  ${hit#${root_dir}/}"
        echo "      use ${instead} instead, or add a waiver comment:"
        echo "      az-iot-allow: ${symbol} -- <reason>"
        echo
    # Match a call, not a mention. The leading class is what keeps `snprintf`
    # from matching inside `vsnprintf`, and is spelled out rather than using \b
    # so the check behaves the same on non-GNU grep. Lines that begin a comment
    # are skipped so prose naming a banned function does not trip the check.
    done < <(grep -rnE "(^|[^[:alnum:]_])${symbol}[[:space:]]*\(" \
                --include='*.c' --include='*.h' "${scan_dir}" 2>/dev/null \
             | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true)
done

if [ "${violations}" -gt 0 ]; then
    echo "${violations} banned construct(s). See c/docs/eng/coding-conventions.md."
    exit 1
fi

echo "c/src is clean: no banned constructs outside documented waivers."
