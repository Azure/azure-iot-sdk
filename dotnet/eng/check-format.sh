#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Fails if .NET code under dotnet/ is not formatted per dotnet/.editorconfig (whitespace and code
# style). Checks Project.slnx plus every project it does not list.
#
#   dotnet/eng/check-format.sh          check
#   dotnet/eng/check-format.sh --fix    reformat in place

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

verify=(--verify-no-changes)
[ "${1:-}" = "--fix" ] && verify=()

targets=(Project.slnx)
while IFS= read -r project; do
    grep -qF "Path=\"${project#./}\"" Project.slnx || targets+=("${project}")
done < <(find . -name '*.csproj' -not -path '*/obj/*' -not -path '*/bin/*' | sort)

for target in "${targets[@]}"; do
    echo "dotnet format: ${target}"
    dotnet format whitespace "${target}" "${verify[@]}"
    dotnet format style "${target}" "${verify[@]}"
done
