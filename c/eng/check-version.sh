#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Checks that every copy of the SDK version matches AZ_IOT_VERSION_STRING in
# inc/azure/iot/az_iot_version.h. With a tag argument (e.g. c/1.0.0-preview),
# also checks the tag and that CHANGELOG.md has a dated entry for it.
# The Yocto PV is excluded: it tracks SRCREV_sdk, which lags main.
# Dependency-free so it can run in the conventions job before configuring.
#
# Usage: eng/check-version.sh [c/<version>]

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
header="${root_dir}/inc/azure/iot/az_iot_version.h"
errors=0

fail() {
    echo "error: $*"
    errors=$((errors + 1))
}

define_value() {
    sed -n -E "s/^#define AZ_IOT_VERSION_$1 \"?([^\"]*)\"?$/\1/p" "${header}"
}

version="$(define_value STRING)"
expected="$(define_value MAJOR).$(define_value MINOR).$(define_value PATCH)"
prerelease="$(define_value PRERELEASE)"
if [ -n "${prerelease}" ]; then
    expected="${expected}-${prerelease}"
fi

# SemVer 2.0.0 without build metadata; keep in sync with CMakeLists.txt.
num='(0|[1-9][0-9]*)'
id='(0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)'
semver="^${num}\.${num}\.${num}(-${id}(\.${id})*)?$"
if ! [[ "${version}" =~ ${semver} ]]; then
    fail "AZ_IOT_VERSION_STRING '${version}' is not a valid SemVer MAJOR.MINOR.PATCH[-PRERELEASE]"
fi
if [ "${version}" != "${expected}" ]; then
    fail "AZ_IOT_VERSION_STRING '${version}' does not match the component macros ('${expected}')"
fi

vcpkg_version="$(sed -n -E 's/^ *"version-semver": "([^"]*)".*/\1/p' "${root_dir}/vcpkg.json")"
if [ "${vcpkg_version}" != "${version}" ]; then
    fail "vcpkg.json version-semver '${vcpkg_version}' != '${version}'"
fi

changelog_top="$(sed -n -E 's/^## ([^ ]+) .*/\1/p' "${root_dir}/CHANGELOG.md" | head -n 1)"
if [ "${changelog_top}" != "${version}" ]; then
    fail "CHANGELOG.md latest entry '${changelog_top}' != '${version}'"
fi

if [ $# -gt 0 ]; then
    if [ "$1" != "c/${version}" ]; then
        fail "tag '$1' != 'c/${version}'"
    fi
    if ! grep -qE "^## ${version//./\\.} \([0-9]{4}-[0-9]{2}-[0-9]{2}\)$" "${root_dir}/CHANGELOG.md"; then
        fail "CHANGELOG.md has no dated entry '## ${version} (YYYY-MM-DD)'"
    fi
fi

if [ "${errors}" -ne 0 ]; then
    exit 1
fi
echo "SDK version ${version}: all copies match."
