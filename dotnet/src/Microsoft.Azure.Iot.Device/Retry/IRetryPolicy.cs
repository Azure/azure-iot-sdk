// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Retry
{
    public interface IRetryPolicy
    {
        /// <summary>
        /// Method called by the client when an operation fails to determine how the failure should be handled,
        /// and how long to wait until retrying the operation.
        /// </summary>
        /// <param name="currentRetryCount">The number of times the current operation has been attempted.</param>
        /// <param name="lastException">The exception that prompted this retry policy check.</param>
        /// <param name="connectionEndpoint">
        /// The endpoint this retry targets, so a policy can tell whether it is being asked to retry connecting to an
        /// IoT hub (<see cref="ConnectionEndpoint.IotHub"/>) or to re-provision and/or reconnect with Device Provisioning Service
        /// (<see cref="ConnectionEndpoint.DeviceProvisioningService"/>) and decide accordingly.
        /// </param>
        /// <param name="retryDelay">
        /// Set this to the desired time to delay before the next attempt. Only consulted when this method returns
        /// <see cref="RetryGuidance.Retry"/> (or <see cref="RetryGuidance.Reprovision"/> for a
        /// Device Provisioning Service endpoint, which is treated the same as <see cref="RetryGuidance.Retry"/>).
        /// </param>
        /// <returns>The <see cref="RetryGuidance"/> describing how the client should handle this failure.</returns>
        /// <example>
        /// <code language="csharp">
        /// class CustomRetryPolicy : IRetryPolicy
        /// {
        ///     public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
        ///     {
        ///         // Add custom logic as needed upon determining how to handle the failure and set the retryDelay out parameter
        ///     }
        /// }
        /// </code>
        /// </example>
        RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay);
    }
}
