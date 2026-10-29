// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    /// <summary>
    /// IoT hub's current version of the desired properties, sent without the properties themselves.
    /// </summary>
    public class DesiredVersionProbeReceivedEventArgs : EventArgs
    {
        /// <summary>
        /// IoT hub's current version of the desired properties.
        /// </summary>
        public required ulong DesiredPropertiesVersion { get; init; }
    }
}
