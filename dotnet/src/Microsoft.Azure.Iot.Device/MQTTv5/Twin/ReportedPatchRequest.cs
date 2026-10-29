// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    /// <summary>
    /// A patch of this device's reported properties.
    /// </summary>
    /// <remarks>
    /// The patch is applied to the reported properties as a JSON merge patch, so a property set to null is removed.
    /// <see cref="ConcurrentTwinClient"/> makes each patch conditional on IoT hub's current version of the reported properties
    /// itself, while <see cref="TwinClient"/> only makes it conditional on the version that the caller provides.
    /// </remarks>
    public class ReportedPatchRequest
    {
        /// <summary>
        /// The reported properties to update.
        /// </summary>
        public required JsonObject ReportedProperties { get; set; }
    }
}