#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Builds the installed-package tests with pkg-config alone (no CMake) and, with
# --run, runs them. Each component's test links that component's .pc file only;
# when adapter_paho is listed, its test is also linked against every listed
# component at once, which checks the link order of a combined command line.
#
#   tests/install/pkg-config-test.sh [--run] <out dir> <component>...
#
# Honours CC, CFLAGS, LDFLAGS and PKG_CONFIG (defaults: cc, none, none, pkg-config),
# so a cross toolchain and a sysroot pkg-config work unchanged.

set -euo pipefail

run=0
if [ "${1:-}" = "--run" ]; then
    run=1
    shift
fi
if [ "$#" -lt 2 ]; then
    echo "usage: $0 [--run] <out dir> <component>..." >&2
    exit 2
fi
out="$1"
shift
src="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc="${CC:-cc}"
pkg_config="${PKG_CONFIG:-pkg-config}"
mkdir -p "$out"

built=()

# build <name> <source> <component> [<component>...]
build() {
    local name="$1" source="$2" flags
    local defines=()
    shift 2
    case "$source" in
        crypto_test.c)
            defines=("-DAZ_IOT_TEST_CRYPTO_HEADER=\"az_iot_$1.h\"" "-DAZ_IOT_TEST_CRYPTO=az_iot_$1")
            ;;
    esac
    flags="$("$pkg_config" --static --cflags --libs "${@/#/azure-iot-sdk-}")"
    echo "$name: $flags"
    # shellcheck disable=SC2086 # CC, CFLAGS, LDFLAGS and pkg-config output are word lists.
    $cc -std=c99 -Wall -Wextra -Wpedantic -Werror ${CFLAGS:-} "${defines[@]}" \
        -o "$out/$name" "$src/$source" ${LDFLAGS:-} $flags
    built+=("$name")
}

for comp in "$@"; do
    case "$comp" in
        crypto_openssl | crypto_mbedtls) source=crypto_test.c ;;
        core | mqttv3 | mqttv5 | adapter_paho | adapter_rust_mqtt | certificate_provider_managed)
            source="${comp}_test.c"
            ;;
        *)
            echo "unknown component: $comp" >&2
            exit 2
            ;;
    esac
    build "pkg_config_test_$comp" "$source" "$comp"
done

for comp in "$@"; do
    if [ "$comp" = adapter_paho ]; then
        # adapter_paho last: its libraries must still precede core's.
        others=()
        for c in "$@"; do
            if [ "$c" != adapter_paho ]; then
                others+=("$c")
            fi
        done
        build pkg_config_test_all adapter_paho_test.c "${others[@]}" adapter_paho
    fi
done

if [ "$run" -eq 1 ]; then
    for name in "${built[@]}"; do
        echo "running $name"
        "$out/$name"
    done
fi
echo "pkg-config tests: ${#built[@]} built, run: $([ "$run" -eq 1 ] && echo yes || echo no)"
