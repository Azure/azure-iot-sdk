// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.Twin.LegacyTwinObjects
{
    /// <summary>
    /// Type that is used to deserialize and represent the received client properties.
    /// This class uses System.Text.Json for the top-level property deserialization
    /// since the property names are known and defined by service contract.
    /// </summary>
    internal sealed class TwinDocument
    {
        /// <summary>
        /// The desired properties for this device
        /// </summary>
        [JsonPropertyName("desired")]
        internal JsonObject Desired { get; set; }

        /// <summary>
        /// The reported properties for this device
        /// </summary>
        [JsonPropertyName("reported")]
        internal JsonObject Reported { get; set; }
    }
}
