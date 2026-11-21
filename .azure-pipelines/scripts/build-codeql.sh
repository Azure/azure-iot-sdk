#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../dotnet"

dotnet build src/Microsoft.Azure.Iot.Device/Microsoft.Azure.Iot.Device.csproj --configuration Release
grep -oE 'Path="samples/[^"]+\.csproj"' Project.slnx | cut -d'"' -f2 | while IFS= read -r project; do
    dotnet build "$project" --configuration Release
done
