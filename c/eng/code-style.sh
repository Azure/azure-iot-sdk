#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# clang-format guardrail for the C project.
#
#   code-style.sh check    fail if any tracked C source differs from c/.clang-format
#   code-style.sh fix      reformat every tracked C source in place
#
# The style is c/.clang-format, adopted from azure-sdk-for-c. Keep it in sync
# when upstream changes.
#
# CLANG_FORMAT pins the binary, and the version matters: output differs between
# major releases, so formatting with a different one than CI uses will fight the
# gate. CI installs clang-format-18. Visual Studio 2022 currently bundles 19, so
# do not rely on the IDE's copy.

set -o errexit
set -o nounset
set -o pipefail

usage() {
    echo "usage: ${0##*/} [check|fix]" 1>&2
    exit 1
}

mode="${1:-}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

# Kept explicit rather than scanning the repo, so that vendored code under
# c/tests/deps and anything fetched into c/build is never touched.
paths=(c/src c/inc c/adapters c/samples c/tests/unit c/tests/e2e c/tests/conformance c/tests/integration c/tests/support c/tests/install)

clang_format="${CLANG_FORMAT:-clang-format}"
if ! command -v "${clang_format}" >/dev/null 2>&1; then
    echo "${clang_format} not found. Install it (apt install clang-format-18) and retry." 1>&2
    exit 1
fi

mapfile -t files < <(git ls-files -- "${paths[@]}" | grep -E '\.(c|h)$' || true)
if [ "${#files[@]}" -eq 0 ]; then
    echo "No C sources found. Check the paths list in ${0##*/}." 1>&2
    exit 1
fi

case "${mode}" in
    check)
        echo "Checking ${#files[@]} files with $("${clang_format}" --version)"
        failed=()
        for file in "${files[@]}"; do
            if ! "${clang_format}" --style=file --dry-run -Werror "${file}" >/dev/null 2>&1; then
                failed+=("${file}")
            fi
        done
        if [ "${#failed[@]}" -gt 0 ]; then
            echo
            echo "${#failed[@]} file(s) do not match c/.clang-format:"
            printf '  %s\n' "${failed[@]}"
            echo
            echo "Run 'bash c/eng/code-style.sh fix' and commit the result."
            exit 1
        fi
        echo "Formatting is clean."
        ;;
    fix)
        "${clang_format}" --style=file -i "${files[@]}"
        echo "Reformatted ${#files[@]} files. Review and commit."
        ;;
    *)
        usage
        ;;
esac
