#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Verifies that every ELF executable and shared library in a build tree carries the hardening from
# cmake/az_iot_hardening.cmake: PIE (executables), full RELRO (GNU_RELRO + BIND_NOW), a
# non-executable stack, and stack protector references in first-party executables. A single
# executable may have no function that needs a canary (compiler heuristics), so that is reported;
# it fails only when no first-party executable has one.
#
#   eng/check-hardening.sh <build-dir>
#
# CMake's own probe binaries (CMakeFiles/) are skipped. Needs readelf (binutils).

set -euo pipefail

[ "$#" -eq 1 ] || { echo "usage: ${0##*/} <build-dir>" 1>&2; exit 1; }
build_dir="$1"
command -v readelf >/dev/null || { echo "readelf not found" 1>&2; exit 1; }

checked=0
failures=0
first_party=0
with_canary=0

fail() {
    echo "FAIL $1: $2"
    failures=$((failures + 1))
}

while IFS= read -r -d '' f; do
    header="$(readelf -h "$f" 2>/dev/null)" || continue   # not ELF
    type="$(awk '/^ *Type:/ {print $2}' <<<"$header")"
    case "$type" in
        EXEC | DYN) ;;
        *) continue ;;
    esac
    shared=0
    case "$f" in *.so | *.so.*) shared=1 ;; esac
    checked=$((checked + 1))

    # Captured, not piped into grep -q: with pipefail an early grep exit would fail the pipe.
    segments="$(readelf -lW "$f")"
    dynamic="$(readelf -dW "$f")"
    symbols="$(readelf -sW "$f")"
    stack="$(grep -E 'GNU_STACK' <<<"$segments" || true)"

    [ "$shared" -eq 1 ] || [ "$type" = DYN ] || fail "$f" "not PIE"
    grep -q 'GNU_RELRO' <<<"$segments" || fail "$f" "no RELRO"
    grep -qE 'BIND_NOW|FLAGS_1.*NOW' <<<"$dynamic" || fail "$f" "no BIND_NOW"
    if ! grep -qE ' RW +0x' <<<"$stack"; then
        fail "$f" "executable or missing GNU_STACK"
    fi
    if [ "$shared" -eq 0 ] && [[ "$f" != */_deps/* ]]; then
        first_party=$((first_party + 1))
        if grep -q '__stack_chk_fail' <<<"$symbols"; then
            with_canary=$((with_canary + 1))
        else
            echo "NOTE $f: no stack protector reference"
        fi
    fi
done < <(find "$build_dir" -path '*/CMakeFiles' -prune -o -type f \( -perm -u+x -o -name '*.so' -o -name '*.so.*' \) -print0)

if [ "$first_party" -gt 0 ] && [ "$with_canary" -eq 0 ]; then
    fail "$build_dir" "no first-party executable references __stack_chk_fail"
fi

echo "hardening: ${checked} ELF files checked, ${with_canary}/${first_party} executables with stack protector, ${failures} failures"
[ "$checked" -gt 0 ] || { echo "no ELF files under ${build_dir}" 1>&2; exit 1; }
[ "$failures" -eq 0 ]
