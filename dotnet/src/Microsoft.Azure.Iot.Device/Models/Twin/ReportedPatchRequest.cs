// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Models.Twin
{
    /// <summary>
    /// A patch of this device's reported properties.
    /// </summary>
    /// <remarks>
    /// The patch is applied to the reported properties as a JSON merge patch, so a property set to null is removed. 
    /// <see cref="MQTTv5.Twin.TwinClient"/> makes each patch conditional on IoT hub's current version of the reported
    /// properties itself. <see cref="Unified.Twin.TwinClient"/> sends every patch unconditionally, since IoT hubs that use
    /// MQTTv3 can't make a patch conditional on a version.
    /// </remarks>
    public class ReportedPatchRequest
    {
        /// <summary>
        /// The reported properties to update.
        /// </summary>
        public required JsonObject ReportedProperties { get; set; }
    }
}
