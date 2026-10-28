// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    /// <summary>
    /// A device's copy of its twin, as kept by <see cref="ConcurrentTwinClient"/>. Serializable with System.Text.Json, so that an
    /// application can persist it and restore it the next time it starts.
    /// </summary>
    /// <remarks>
    /// <para>
    /// IoT hub's versions are never 0, so a version of 0 means the device doesn't hold that section in full. The properties of
    /// a section that isn't held in full may still reflect the changes that this device knows of.
    /// </para>
    /// <para>
    /// <see cref="Reported"/> only contains the reported properties patches that IoT hub has applied. The patches that it
    /// hasn't applied yet are in <see cref="PendingReportedPatches"/>, in the order they were made.
    /// </para>
    /// </remarks>
    public sealed class TwinState
    {
        /// <summary>
        /// The version of <see cref="Desired"/>, or 0 if the device doesn't hold the desired properties in full.
        /// </summary>
        public ulong DesiredVersion { get; init; }

        /// <summary>
        /// The desired properties, as of <see cref="DesiredVersion"/>.
        /// </summary>
        public JsonObject Desired { get; init; } = new();

        /// <summary>
        /// The version of <see cref="Reported"/>, or 0 if the device doesn't hold the reported properties in full.
        /// </summary>
        public ulong ReportedVersion { get; init; }

        /// <summary>
        /// The reported properties that IoT hub has applied, as of <see cref="ReportedVersion"/>.
        /// </summary>
        public JsonObject Reported { get; init; } = new();

        /// <summary>
        /// The reported properties patches that IoT hub hasn't applied yet, in the order they were made.
        /// </summary>
        public IReadOnlyList<JsonObject> PendingReportedPatches { get; init; } = [];
    }
}
