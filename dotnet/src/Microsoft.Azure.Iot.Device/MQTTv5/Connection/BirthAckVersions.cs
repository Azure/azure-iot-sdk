// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.MQTTv5.Connection
{
    /// <summary>
    /// The twin versions that IoT hub revealed in the birth ack for a connection.
    /// </summary>
    /// <param name="ConnectionNonce">The connection nonce of the birth that this birth ack acknowledged.</param>
    /// <param name="DesiredVersion">IoT hub's version of the desired properties, or 0 if it wasn't included.</param>
    /// <param name="ReportedVersion">IoT hub's version of the reported properties, or 0 if it wasn't included.</param>
    internal sealed record BirthAckVersions(Guid ConnectionNonce, ulong DesiredVersion, ulong ReportedVersion);
}
