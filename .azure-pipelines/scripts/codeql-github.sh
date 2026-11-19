#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

set -euo pipefail
cd "$BUILD_SOURCESDIRECTORY"

version=2.27.4
archive="$AGENT_TEMPDIRECTORY/codeql-bundle-linux64.tar.gz"
curl --fail --silent --show-error --location --retry 3 \
    "https://github.com/github/codeql-action/releases/download/codeql-bundle-v${version}/codeql-bundle-linux64.tar.gz" \
    --output "$archive"
echo "89ed56c8ec6bfccef6a0d2e7fe86da0469ca24b4db08b88016ec134b942c17a4  $archive" | sha256sum --check
tar -xzf "$archive" -C "$AGENT_TEMPDIRECTORY"

codeql="$AGENT_TEMPDIRECTORY/codeql/codeql"
mkdir -p "$BUILD_ARTIFACTSTAGINGDIRECTORY/codeql"
"$codeql" database create "$AGENT_TEMPDIRECTORY/codeql-database" \
    --language=csharp --source-root="$BUILD_SOURCESDIRECTORY" \
    --command="bash .azure-pipelines/scripts/build-codeql.sh"
"$codeql" database analyze "$AGENT_TEMPDIRECTORY/codeql-database" \
    codeql/csharp-queries:codeql-suites/csharp-security-extended.qls \
    --format=sarif-latest --sarif-category=/language:csharp \
    --output="$BUILD_ARTIFACTSTAGINGDIRECTORY/codeql/results.sarif" \
    --threads=0
