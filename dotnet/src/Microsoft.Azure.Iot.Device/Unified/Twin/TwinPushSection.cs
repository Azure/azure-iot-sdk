// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Unified.Twin
{
    /// <summary>
    /// A full copy of one section of the twin.
    /// </summary>
    public class TwinPushSection
    {
        /// <summary>
        /// The properties of the section.
        /// </summary>
        public required JsonObject Properties { get; set; }

        /// <summary>
        /// The version of <see cref="Properties"/>.
        /// </summary>
        public required ulong PropertiesVersion { get; set; }
    }
}
