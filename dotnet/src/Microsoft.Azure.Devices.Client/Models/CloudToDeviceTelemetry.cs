
namespace Microsoft.Azure.Devices.Client.Models
{
    public class CloudToDeviceTelemetry
    {
        /// <summary>
        /// Creates an instance of this class.
        /// </summary>
        /// <remarks>
        /// This class can be inherited from and set by unit tests for mocking purposes.
        /// </remarks>
        /// <param name="payload">The payload to be set to the incoming message.</param>
        protected internal CloudToDeviceTelemetry(byte[] payload)
        {
            Payload = payload;
        }

        /// <summary>
        /// The message payload
        /// </summary>
        public byte[] Payload { get; set; }

        public string MessageId { get; set; }

        public string CorrelationId { get; set; }

        /// <summary>
        /// Used to specify the content type of the message.
        /// </summary>
        public string ContentType { get; set; }

        /// <summary>
        /// Used to specify the content encoding type of the message.
        /// </summary>
        public string ContentEncoding { get; set; }

        public Dictionary<string, string> UserProperties { get; } = new();
    }
}
