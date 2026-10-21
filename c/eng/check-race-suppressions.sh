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
#
# Usage: check-race-suppressions.sh [file.supp...]   (default: the two above)
#        check-race-suppressions.sh --self-test      (fixtures in eng/sanitizers/selftest)

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" = "--self-test" ]; then
    failures=0
    for f in "${root_dir}"/eng/sanitizers/selftest/*.supp; do
        if bash "$0" "${f}" >/dev/null 2>&1; then verdict=pass; else verdict=fail; fi
        case "$(basename "${f}")" in
            good-*) expected=pass ;;
            *) expected=fail ;;
        esac
        if [ "${verdict}" != "${expected}" ]; then
            echo "self-test: ${f#"${root_dir}"/}: expected ${expected}, got ${verdict}"
            failures=$((failures + 1))
        fi
    done
    [ "${failures}" -eq 0 ] || exit 1
    echo "self-test: every fixture gave the expected verdict."
    exit 0
fi

# Functions defined in first-party sources (.c and .h, which hold inline code),
# in either clang-format shape:
# - a name followed by "(" on a line that starts at column 0, after its return
#   type and any qualifiers or macros (AZ_NODISCARD ...);
# - a signature, at any indentation and possibly split over lines, whose body
#   "{" stands alone at the same indentation (control statements excluded).
mapfile -t first_party < <(
    {
        # shellcheck disable=SC2016 # $0 and friends belong to awk
        find "${root_dir}/src" "${root_dir}/adapters" \( -name '*.c' -o -name '*.h' \) -print0 |
            xargs -0 awk '
                function indent_of(s) { match(s, /^[ \t]*/); return RLENGTH }
                /^[ \t]*\{[ \t]*$/ {
                    if (sig != "" && indent_of($0) == sig_indent && sig !~ /=/ \
                        && match(sig, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
                        name = substr(sig, RSTART, RLENGTH)
                        sub(/[ \t]*\($/, "", name)
                        print name
                    }
                    sig = ""
                    next
                }
                /^[ \t]*$/ || /^[ \t]*#/ || /^[ \t]*\// || /^[ \t]*\*/ || /;[ \t]*$/ || /^[ \t]*\}/ { sig = ""; next }
                /^[ \t]*(if|else|for|while|switch|do|return|case|default)([^A-Za-z0-9_]|$)/ { sig = ""; next }
                sig == "" && /^[ \t]*[A-Za-z_]/ { sig = $0; sig_indent = indent_of($0); next }
                sig != "" { sig = sig " " $0 }'
        grep -rhoE --include='*.c' --include='*.h' \
            '^([A-Za-z_][A-Za-z0-9_]*[ *]+)+\**[A-Za-z_][A-Za-z0-9_]*\(' \
            "${root_dir}/src" "${root_dir}/adapters" |
            sed -E 's/\($//; s/.*[ *]//'
    } | sort -u
)

violations=0
fail() {
    echo "$1:$2: $3"
    violations=$((violations + 1))
}

if [ "$#" -gt 0 ]; then
    files=("$@")
else
    files=("${root_dir}/eng/sanitizers/helgrind.supp" "${root_dir}/eng/sanitizers/drd.supp")
fi

for supp in "${files[@]}"; do
    [ -f "${supp}" ] || continue
    rel="${supp#"${root_dir}"/}"
    prev=""
    in_entry=0
    line_no=0
    start=0
    # Significant lines seen so far in the entry: 1 = name, 2 = kind, 3.. =
    # frames. Blank lines and comments are not counted: valgrind ignores them.
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
        if [ -z "${line}" ] || [[ "${line}" == \#* ]]; then
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
