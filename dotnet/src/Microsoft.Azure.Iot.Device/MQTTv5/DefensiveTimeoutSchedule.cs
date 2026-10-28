// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.MQTTv5
{
    /// <summary>
    /// The escalating, jittered schedule of defensive timeouts that IoT hub prescribes for exchanges that should complete
    /// on a healthy connection (for example, an expected twin push).
    /// </summary>
    internal static class DefensiveTimeoutSchedule
    {
        /// <summary>
        /// Get how long to wait for the given attempt (starting at 1) of an exchange before recovering.
        /// </summary>
        public static TimeSpan GetTimeout(int attempt)
        {
            int baseMinutes = attempt switch
            {
                1 => 5,
                2 => 6,
                3 => 8,
                _ => 10,
            };

            // The wide jitter keeps devices that time out together from all recovering in lockstep.
            return TimeSpan.FromMinutes(baseMinutes) + TimeSpan.FromMinutes(5 * Random.Shared.NextDouble());
        }
    }
}
