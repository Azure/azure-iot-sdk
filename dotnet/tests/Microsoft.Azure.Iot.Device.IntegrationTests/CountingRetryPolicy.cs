// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Retry;

namespace Microsoft.Azure.Iot.Device.IntegrationTests
{
    /// <summary>
    /// Wraps another retry policy and, using the endpoint the client passes to each retry check, counts how many of
    /// those checks were for Device Provisioning Service and how many advised abandoning the hub to re-provision. This
    /// lets a test tell re-provisioning retries apart from hub reconnection retries and assert on how many times the
    /// device fell back to DPS.
    /// </summary>
    internal sealed class CountingRetryPolicy : IRetryPolicy
    {
        private readonly IRetryPolicy _inner;
        private int _deviceProvisioningServiceConsultations;
        private int _reprovisionGuidances;

        public CountingRetryPolicy(IRetryPolicy inner) => _inner = inner;

        /// <summary>How many times this policy has been consulted for a Device Provisioning Service retry.</summary>
        public int DeviceProvisioningServiceConsultations => Volatile.Read(ref _deviceProvisioningServiceConsultations);

        /// <summary>How many times this policy advised abandoning the hub and re-provisioning through DPS.</summary>
        public int ReprovisionGuidances => Volatile.Read(ref _reprovisionGuidances);

        public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
        {
            if (connectionEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                Interlocked.Increment(ref _deviceProvisioningServiceConsultations);
            }

            RetryGuidance guidance = _inner.GetRetryGuidance(currentRetryCount, lastException, connectionEndpoint, out retryDelay);

            if (guidance == RetryGuidance.Reprovision)
            {
                Interlocked.Increment(ref _reprovisionGuidances);
            }

            return guidance;
        }
    }
}
