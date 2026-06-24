// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using Xunit;

namespace Azure.Iot.Sdk.C.E2E;

/// <summary>
/// Device Update (ADU) end-to-end scenarios.
///
/// ADU is intentionally a SEPARATE category: provisioning the Device Update
/// account/instance takes ~25 minutes, so these tests are excluded from PRs and
/// run only on the nightly schedule, on manual dispatch, or when a PR touches
/// ADU code paths (see ci-c-e2e.yml). The fast PR job runs Category=Fast only;
/// this job runs Category=Adu.
///
/// This placeholder keeps the Adu category non-empty (so `dotnet test --filter
/// Category=Adu` reports a clean, intentional skip) until the real ADU
/// scenarios are added. Clone the telemetry test for the device-agent pattern.
/// </summary>
[Trait("Category", "Adu")]
public sealed class AduE2ETests
{
    [Fact]
    public void Adu_Deployment_Placeholder()
    {
        Assert.Skip("ADU e2e scenarios not yet implemented (infrastructure only).");
    }
}
