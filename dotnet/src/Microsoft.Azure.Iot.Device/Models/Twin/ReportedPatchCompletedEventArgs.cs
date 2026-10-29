// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Models.Twin
{
    /// <summary>
    /// The outcome of a reported properties patch that <see cref="Unified.Twin.TwinClient"/> or <see cref="MQTTv5.Twin.TwinClient"/> sent to IoT hub.
    /// </summary>
    public class ReportedPatchCompletedEventArgs : EventArgs
    {
        /// <summary>
        /// The patch that was sent. This combines every reported properties patch made while an earlier one was in flight.
        /// </summary>
        public required JsonObject Patch { get; init; }

        /// <summary>
        /// IoT hub's response to the patch, or null if the patch couldn't be sent (see <see cref="Error"/>).
        /// </summary>
        public ReportedPatchResponse? Response { get; init; }

        /// <summary>
        /// Why the patch couldn't be sent, or null if IoT hub responded to it.
        /// </summary>
        public Exception? Error { get; init; }
    }
}
