// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// Optional settings for disconnecting a connection client.
    /// </summary>
    public class DisconnectOptions
    {
        /// <summary>
        /// The session expiry interval, in seconds, to include in the MQTT DISCONNECT. Defaults to 3600 (one hour),
        /// matching the C SDK's <c>AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS</c>.
        /// </summary>
        /// <remarks>
        /// This setting only applies to MQTT 5 connections. The default MQTT adapter omits a value of 0,
        /// leaving the session expiry interval established at CONNECT unchanged. The broker may restrict which
        /// nonzero values it accepts; MQTT 5 forbids increasing the interval from 0 at DISCONNECT.
        /// </remarks>
        public uint SessionExpiryInterval { get; set; } = 3600;
    }
}
