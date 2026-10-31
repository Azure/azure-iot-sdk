// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;

namespace Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods
{
    /// <summary>
    /// The device's answer to a direct method probe: whether it is ready to execute the method.
    /// </summary>
    public class DirectMethodProbeAck
    {
        private DirectMethodProbeAck(ProbeAck probeAck, TimeSpan minimumExecutionTime)
        {
            ProbeAck = probeAck;
            MinimumExecutionTime = minimumExecutionTime;
        }

        internal ProbeAck ProbeAck { get; }

        // The least time the method needs to run. The client refuses to start it with less time than this left.
        internal TimeSpan MinimumExecutionTime { get; }

        /// <summary>
        /// Accept the probe, declaring that the device is ready to execute the method.
        /// </summary>
        /// <returns>A probe ack that accepts the probe.</returns>
        public static DirectMethodProbeAck Accepted()
        {
            return Accepted(TimeSpan.Zero);
        }

        /// <summary>
        /// Accept the probe, declaring that the device is ready to execute the method and the least time it needs to do so.
        /// </summary>
        /// <param name="minimumExecutionTime">
        /// The least time the method needs to run. If the probe's response timeout, less the safety margin, is not more than this,
        /// the client rejects the probe with <see cref="RejectedReason.InsufficientTime"/>. If less than this is left when the
        /// exec message arrives, the client abandons the invocation without running the method.
        /// </param>
        /// <returns>A probe ack that accepts the probe.</returns>
        /// <exception cref="ArgumentOutOfRangeException">Thrown if <paramref name="minimumExecutionTime"/> is negative.</exception>
        public static DirectMethodProbeAck Accepted(TimeSpan minimumExecutionTime)
        {
            ArgumentOutOfRangeException.ThrowIfLessThan(minimumExecutionTime, TimeSpan.Zero, nameof(minimumExecutionTime));

            return new DirectMethodProbeAck(
                new ProbeAck
                {
                    Ready = new()
                    {
                        // Generate a ready Id for the user b/c there is no particular reason that they would care about what value this is
                        ReadyId = ByteString.CopyFrom(Guid.NewGuid().ToByteArray())
                    }
                },
                minimumExecutionTime);
        }

        /// <summary>
        /// Reject the probe, so the service fails the invocation immediately instead of sending the method.
        /// </summary>
        /// <param name="reason">Why the device can't execute the method.</param>
        /// <returns>A probe ack that rejects the probe.</returns>
        public static DirectMethodProbeAck Rejected(RejectedReason reason)
        {
            return new DirectMethodProbeAck(
                new ProbeAck
                {
                    Rejected = new()
                    {
                        Reason = reason,
                    }
                },
                TimeSpan.Zero);
        }
    }
}
