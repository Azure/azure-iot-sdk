#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Verifies that the executables and shared libraries in a build tree carry the hardening from
# cmake/az_iot_hardening.cmake.
#
# ELF (readelf): PIE (executables), full RELRO (GNU_RELRO + BIND_NOW), a non-executable stack,
# and stack protector references in first-party executables. A single executable may have no
# function that needs a canary (compiler heuristics), so that is reported; it fails only when no
# first-party executable has one.
#
# PE (dumpbin, on Windows): Control Flow Guard (the Control Flow Guard DLL characteristic, CF
# instrumented and FID table present), ASLR (Dynamic base), DEP (NX compatible) and, for x64,
# High Entropy VA and CET compatible.
#
#   eng/check-hardening.sh <build-dir>
#
# CMake's own probe binaries (CMakeFiles/) and fetched tools (_deps/vcpkg-*) are skipped.

set -euo pipefail

[ "$#" -eq 1 ] || { echo "usage: ${0##*/} <build-dir>" 1>&2; exit 1; }
build_dir="$1"

checked=0
failures=0
first_party=0
with_canary=0

fail() {
    echo "FAIL $1: $2"
    failures=$((failures + 1))
    first_failed="${first_failed:-$1}"
}

check_pe() {
    command -v dumpbin >/dev/null || { echo "dumpbin not found (run from a VS developer shell)" 1>&2; exit 1; }
    while IFS= read -r -d '' f; do
        checked=$((checked + 1))
        # Dash options: Git Bash rewrites arguments that start with '/'. dumpbin writes CRLF.
        info="$(dumpbin -nologo -headers -loadconfig "$f" | tr -d '\r')"
        grep -q 'Dynamic base' <<<"$info" || fail "$f" "no ASLR (Dynamic base)"
        grep -q 'NX compatible' <<<"$info" || fail "$f" "no DEP (NX compatible)"
        grep -qE '^[[:space:]]+Control Flow Guard[[:space:]]*$' <<<"$info" \
            || fail "$f" "no Control Flow Guard characteristic"
        grep -qi 'CF Instrumented' <<<"$info" || fail "$f" "not CF instrumented"
        grep -qi 'FID table present' <<<"$info" || fail "$f" "no CFG function table"
        if grep -q 'machine (x64)' <<<"$info"; then
            grep -q 'High Entropy Virtual Addresses' <<<"$info" || fail "$f" "no high entropy VA"
            grep -qi 'CET compatible' <<<"$info" || fail "$f" "not CET compatible"
        fi
    done < <(find "$build_dir" \( -path '*/CMakeFiles' -o -path '*/_deps/vcpkg-*' \) -prune \
        -o -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)

    if [ "$failures" -gt 0 ] && [ -n "${first_failed:-}" ]; then
        echo "dumpbin output for ${first_failed}:"
        dumpbin -nologo -headers -loadconfig "$first_failed" | tr -d '\r' \
            | grep -iE 'machine|characteristics|Dynamic base|NX compatible|Guard|CF |FID|CET|High Entropy' || true
    fi
    echo "hardening: ${checked} PE files checked, ${failures} failures"
    [ "$checked" -gt 0 ] || { echo "no PE files under ${build_dir}" 1>&2; exit 1; }
    [ "$failures" -eq 0 ]
}

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) check_pe; exit ;;
esac

command -v readelf >/dev/null || { echo "readelf not found" 1>&2; exit 1; }

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
done < <(find "$build_dir" \( -path '*/CMakeFiles' -o -path '*/_deps/vcpkg-*' \) -prune \
    -o -type f \( -perm -u+x -o -name '*.so' -o -name '*.so.*' \) -print0)

if [ "$first_party" -gt 0 ] && [ "$with_canary" -eq 0 ]; then
    fail "$build_dir" "no first-party executable references __stack_chk_fail"
fi

echo "hardening: ${checked} ELF files checked, ${with_canary}/${first_party} executables with stack protector, ${failures} failures"
[ "$checked" -gt 0 ] || { echo "no ELF files under ${build_dir}" 1>&2; exit 1; }
[ "$failures" -eq 0 ]
