// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    /// <summary>
    /// Raised when the connection layer gives up on a connection-level error because it is not retryable.
    /// </summary>
    /// <remarks>
    /// This layer swallows and retries every retryable connection-level error, so this event only fires for
    /// errors classified as <see cref="ErrorRetryability.Terminal"/> or <see cref="ErrorRetryability.IdentityTerminal"/>.
    /// Once it fires, the connection layer has stopped maintaining the connection and will not reconnect on its own.
    /// </remarks>
    public class MqttConnectionFaultedEventArgs : EventArgs
    {
        /// <summary>
        /// The classified error that ended connection maintenance.
        /// </summary>
        public required DeviceException Exception { get; init; }

        /// <summary>
        /// Whether the fault is attributable to the device's identity or credential, meaning a DPS-provisioned
        /// device should re-provision rather than fault outright.
        /// </summary>
        public bool IsIdentityFault => Exception.Retryability == ErrorRetryability.IdentityTerminal;

        /// <summary>
        /// The disconnect that preceded the fault, if the fault followed an unexpected disconnect rather than a
        /// rejected connect attempt.
        /// </summary>
        public MqttClientDisconnectedEventArgs? LastDisconnect { get; init; }
    }
}
