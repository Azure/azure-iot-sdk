#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Guards eng/sanitizers/{helgrind,drd}.supp: a suppression may hide a race in
# third-party code, never in the SDK's own. Each entry must:
# - follow a comment line (its upstream reference);
# - start with a concrete frame, not "...";
# - name no function defined in c/src or c/adapters (globs are expanded);
# - use obj: only for shared libraries (*.so*): the SDK is linked statically.
# Dependency-free so it runs in the conventions job before configure.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
violations=0

# Functions defined in first-party sources: clang-format puts the name of a
# definition at the start of a line, after its return type.
mapfile -t first_party < <(
    grep -rhoE --include='*.c' \
        '^(static )?(const )?[A-Za-z_][A-Za-z0-9_]*[ *]+\**[A-Za-z_][A-Za-z0-9_]*\(' \
        "${root_dir}/src" "${root_dir}/adapters" |
        sed -E 's/\($//; s/.*[ *]//' | sort -u
)

fail() {
    echo "$1:$2: $3"
    violations=$((violations + 1))
}

for supp in "${root_dir}"/eng/sanitizers/helgrind.supp "${root_dir}"/eng/sanitizers/drd.supp; do
    [ -f "${supp}" ] || continue
    rel="${supp#"${root_dir}"/}"
    prev=""
    in_entry=0
    line_no=0
    start=0
    # Body lines seen so far in the entry: 1 = name, 2 = kind, 3.. = frames.
    body=0
    while IFS= read -r raw || [ -n "${raw}" ]; do
        line_no=$((line_no + 1))
        line="$(sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//' <<<"${raw}")"
        if [ "${in_entry}" -eq 0 ]; then
            if [ "${line}" = "{" ]; then
                in_entry=1
                start=${line_no}
                body=0
                [[ "${prev}" == \#* ]] || fail "${rel}" "${line_no}" "entry has no comment before it"
            fi
            [ -z "${line}" ] || prev="${line}"
            continue
        fi
        if [ "${line}" = "}" ]; then
            in_entry=0
            [ "${body}" -ge 3 ] || fail "${rel}" "${start}" "entry has no frame"
            prev="}"
            continue
        fi
        body=$((body + 1))
        [ "${body}" -ge 3 ] || continue
        if [ "${body}" -eq 3 ] && [ "${line}" = "..." ]; then
            fail "${rel}" "${line_no}" "first frame is '...'"
        fi
        case "${line}" in
            fun:*)
                pattern="${line#fun:}"
                for f in "${first_party[@]}"; do
                    # shellcheck disable=SC2053 # glob match is the point
                    if [[ "${f}" == ${pattern} ]]; then
                        fail "${rel}" "${line_no}" "'${line}' matches SDK function ${f}"
                        break
                    fi
                done
                ;;
            obj:*)
                [[ "${line}" == *.so* ]] || fail "${rel}" "${line_no}" "'${line}' is not a shared library"
                ;;
            src:*)
                fail "${rel}" "${line_no}" "src: frames are not allowed; use fun:"
                ;;
        esac
    done <"${supp}"
    [ "${in_entry}" -eq 0 ] || fail "${rel}" "${start}" "unterminated entry"
done

if [ "${violations}" -gt 0 ]; then
    echo "${violations} race-suppression violation(s). See the header of eng/sanitizers/helgrind.supp."
    exit 1
fi
echo "race suppressions name third-party code only (${#first_party[@]} SDK functions checked)."
