#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Fails if any project under dotnet/ references a NuGet package, direct or transitive, with a
# known vulnerability (NuGet advisory data). Every .csproj is checked, including projects that
# Project.slnx does not list.
#
#   dotnet/eng/check-vulnerable-packages.sh

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v jq >/dev/null || { echo "jq not found" 1>&2; exit 1; }

mapfile -t projects < <(find "${root_dir}" -name '*.csproj' -not -path '*/obj/*' -not -path '*/bin/*' | sort)
[ "${#projects[@]}" -gt 0 ] || { echo "no projects under ${root_dir}" 1>&2; exit 1; }

found=0
for project in "${projects[@]}"; do
    dotnet restore "${project}" --verbosity quiet
    report="$(dotnet list "${project}" package --vulnerable --include-transitive --format json)"
    hits="$(jq -r '
        .projects[]? | .frameworks[]? | ((.topLevelPackages // []) + (.transitivePackages // []))[]
        | select((.vulnerabilities // []) | length > 0)
        | . as $p | .vulnerabilities[]
        | "\($p.id) \($p.resolvedVersion): \(.severity) \(.advisoryurl)"' <<<"${report}" | sort -u)"
    if [ -n "${hits}" ]; then
        echo "${project#"${root_dir}/"}:"
        sed 's/^/  /' <<<"${hits}"
        found=1
    fi
done

if [ "${found}" -ne 0 ]; then
    echo "Vulnerable packages found. Update them (a transitive one can be pinned with a direct PackageReference)."
    exit 1
fi
echo "No known vulnerable packages in ${#projects[@]} projects."
