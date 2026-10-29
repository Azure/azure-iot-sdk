#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build and install an OpenSSL release (shared libraries, headers, CLI, openssl.cnf) from its source
# tarball, for the OpenSSL 4.x CI legs. No distribution or runner image ships 4.x yet.
#
#   eng/install-openssl.sh <version> <prefix>
#
# Then configure with -DOPENSSL_ROOT_DIR=<prefix> (or the OPENSSL_ROOT_DIR environment variable)
# and, on Windows, put <prefix>/bin on PATH for the DLLs. Linux libraries carry an RPATH to
# <prefix>/lib. Only the versions pinned below are accepted; add a line (with the release's
# published SHA-256) to test another.
#
# Linux: a C compiler, make and perl. Windows (Git Bash): an MSVC developer environment (cl, nmake)
# and Strawberry Perl, which OpenSSL needs for VC targets (Git's MSYS perl is not supported).
# Assembly is disabled on Windows, so NASM is not needed.

set -euo pipefail

usage() {
    echo "usage: ${0##*/} <version> <prefix>" 1>&2
    exit 1
}

[ "$#" -eq 2 ] || usage
version="$1"
prefix="$2"

case "${version}" in
    4.0.3) sha256="325b5c806167c13b40b1ffeadfe0248197c00eccc4cf123ec1e28d2d2fd216d9" ;;
    4.1.0) sha256="50c17baf54cb008f6ce42d50deb4b28050a1aad711636cabd4b241c7e90cc2fd" ;;
    *) echo "OpenSSL ${version} is not pinned in ${0##*/}" 1>&2; exit 1 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

archive="openssl-${version}.tar.gz"
curl -sSfL -o "${work}/${archive}" \
    "https://github.com/openssl/openssl/releases/download/openssl-${version}/${archive}"
echo "${sha256}  ${work}/${archive}" | sha256sum -c -
tar -xzf "${work}/${archive}" -C "${work}"
cd "${work}/openssl-${version}"

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        perl="/c/Strawberry/perl/bin/perl.exe"
        [ -x "${perl}" ] || { echo "Strawberry Perl not found at ${perl}" 1>&2; exit 1; }
        command -v cl >/dev/null || { echo "cl not found: set up the MSVC environment first" 1>&2; exit 1; }
        # MSVC's link.exe must win over Git's /usr/bin/link, which nmake would otherwise run.
        PATH="$(dirname "$(command -v cl)"):${PATH}"
        win_prefix="$(cygpath -w "${prefix}")"
        "${perl}" Configure VC-WIN64A no-asm no-docs no-tests \
            --prefix="${win_prefix}" --openssldir="${win_prefix}\\ssl"
        nmake
        nmake install_sw install_ssldirs
        ;;
    *)
        # $(LIBRPATH) is expanded by OpenSSL's Makefile to <prefix>/lib.
        # shellcheck disable=SC2016
        ./Configure no-docs no-tests --prefix="${prefix}" --libdir=lib '-Wl,-rpath,$(LIBRPATH)'
        make -j"$(nproc 2>/dev/null || echo 2)"
        make install_sw install_ssldirs
        ;;
esac
