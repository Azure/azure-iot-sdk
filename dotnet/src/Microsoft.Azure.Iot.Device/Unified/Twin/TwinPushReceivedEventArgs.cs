// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    /// <summary>
    /// A full copy of one or both sections of the twin that is newer than (or replaces) the device's copy.
    /// </summary>
    /// <remarks>
    /// Classic IoT hub connections don't push the twin, so <see cref="TwinClient"/> raises this event when the twin
    /// that it gets after each connection, or after detecting a missed desired properties patch, changes its copy.
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
