# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Prints the resource group name of an e2e suite's run: <prefix>-Ado-<run id>. Setup and teardown
# both compute it, so teardown does not depend on setup outputs.
param(
    [Parameter(Mandatory)][string]$Suite,
    [Parameter(Mandatory)][string]$RunId
)

$prefix = switch ($Suite) {
    'standard' { 'DotnetE2ETestPipeline' }
    'csr' { 'DotnetCertMgmtE2ETestPipeline' }
    'longhaul' { 'DotnetLonghaul' }
    'c-e2e' { 'CSDKE2E' }
    default { throw "Unknown test suite '$Suite'." }
}
"$prefix-Ado-$RunId"
