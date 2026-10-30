// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models.DirectMethods
{
    public class DirectMethodRequestReceivedEventArgs : EventArgs
    {
        public byte[]? Payload { get; init; }

        public required string MethodName { get; init; }

        /// <summary>
        /// Cancelled when the service's response timeout for this direct method elapses, after which any response would be discarded.
        /// </summary>
        /// <remarks>
        /// Only MQTTv5 IoT hubs communicate a response timeout. For other hubs this is <see cref="CancellationToken.None"/>.
        /// </remarks>
        public CancellationToken CancellationToken { get; init; }
    }
}
