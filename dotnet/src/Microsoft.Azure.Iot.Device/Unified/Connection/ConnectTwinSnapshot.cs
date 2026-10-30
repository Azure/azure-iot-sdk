// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Unified.Connection
{
    /// <summary>
    /// The twin that a connection client got from IoT hub as part of the device presence flow of one connection, the
    /// MQTTv3 counterpart of the twin that IoT hubs that use MQTTv5 push to the device when it connects.
    /// </summary>
    /// <param name="ConnectionId">
    /// The <see cref="IConnectionClient.HubConnectionId"/> of the connection that this twin was fetched on. A snapshot whose
    /// ID is not the current one is from a previous connection.
    /// </param>
    /// <param name="Desired">The desired properties, without their "$version" entry. Must not be modified.</param>
    /// <param name="DesiredVersion">The version of the desired properties, or 0 if IoT hub didn't include one.</param>
    /// <param name="Reported">The reported properties, without their "$version" entry. Must not be modified.</param>
    /// <param name="ReportedVersion">The version of the reported properties, or 0 if IoT hub didn't include one.</param>
    internal sealed record ConnectTwinSnapshot(long ConnectionId, JsonObject Desired, ulong DesiredVersion, JsonObject Reported, ulong ReportedVersion);
}
