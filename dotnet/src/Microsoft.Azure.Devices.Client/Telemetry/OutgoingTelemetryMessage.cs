using System.Buffers;

namespace Microsoft.Azure.Devices.Client.Telemetry
{
    /// <summary>
    /// A single device-to-cloud telemetry message.
    /// </summary>
    public class OutgoingTelemetryMessage
    {
        //TODO do we want configurable QoS here?

        /// <summary>
        /// The optional payload of the outgoing telemetry.
        /// </summary>
        public ArraySegment<byte> PayloadAsArraySegment
        {
            get
            {
                if (PayloadAsReadOnlySequence.IsEmpty)
                {
                    return ArraySegment<byte>.Empty;
                }

                return new ArraySegment<byte>(PayloadAsReadOnlySequence.ToArray()); //TOOD perf considerations?
            }
            set
            {
                PayloadAsReadOnlySequence = new(value);
            }
        }

        /// <summary>
        /// The optional payload of the outgoing telemetry.
        /// </summary>
        public ReadOnlySequence<byte> PayloadAsReadOnlySequence { get; set; }

        /// <summary>
        /// The optional payload of the outgoing telemetry.
        /// </summary>
        public byte[] Payload
        {
            get
            {
                if (PayloadAsReadOnlySequence.IsEmpty)
                {
                    return Array.Empty<byte>();
                }

                return PayloadAsReadOnlySequence.ToArray();
            }
            set
            {
                PayloadAsReadOnlySequence = new(value);
            }
        }

        /// <summary>
        /// The message Id for this telemetry message
        /// </summary>
        public string? MessageId { get; set; }

        /// <summary>
        /// The correlation Id for this telemetry message
        /// </summary>
        public string? CorrelationId { get; set; }

        /// <summary>
        /// The content type of this telemetry message's payload. Should only be set if <see cref="PayloadAsReadOnlySequence"/> is not empty.
        /// </summary>
        public string? ContentType { get; set; }

        /// <summary>
        /// The content encoding of this telemetry message's payload. Should only be set if <see cref="PayloadAsReadOnlySequence"/> is not empty.
        /// </summary>
        public string? ContentEncoding { get; set; }

        /// <summary>
        /// Custom user properties to include with this telemetry message.
        /// </summary>
        public Dictionary<string, string> UserProperties { get; set; } = new();
    }
}
