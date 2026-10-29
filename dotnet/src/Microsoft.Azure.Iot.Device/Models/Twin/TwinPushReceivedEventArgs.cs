// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Models.Twin
{
    /// <summary>
    /// A full copy of one or both sections of the twin that is newer than (or replaces) the device's copy.
    /// </summary>
    /// <remarks>
    /// IoT hubs that use MQTTv5 push the sections of the twin that changed when the device connects. IoT hubs that use
    /// MQTTv3 don't push the twin, so <see cref="Unified.Twin.TwinClient"/> raises this event when the twin that it gets
    /// after each connection to such an IoT hub changes its copy. Either client also raises it when it resyncs the desired
    /// properties after detecting a missed desired properties patch.
    /// </remarks>
    public class TwinPushReceivedEventArgs : EventArgs
    {
        /// <summary>
        /// The desired properties, or null if the device's copy of them didn't change.
        /// </summary>
        public TwinPushSection? Desired { get; set; }

        /// <summary>
        /// The reported properties, or null if the device's copy of them didn't change.
        /// </summary>
        public TwinPushSection? Reported { get; set; }
    }
}
