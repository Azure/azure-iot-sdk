// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using System;

namespace Microsoft.Azure.Iot.Device.Models
{
    /// <summary>
    /// Raised when this client has permanently stopped maintaining its connection to IoT hub.
    /// </summary>
    /// <remarks>
    /// This client retries every retryable connection error on its own, and re-provisions on its own when the
    /// device was provisioned through DPS and the error was attributable to its identity. This event therefore
    /// only fires once no automatic recovery remains, and the connection will not be re-established unless the
    /// application connects again itself.
    /// </remarks>
    public class ConnectionFaultedEventArgs : EventArgs
    {
        /// <summary>
        /// The error that ended the connection.
        /// </summary>
        public required DeviceException Exception { get; init; }
    }
}
