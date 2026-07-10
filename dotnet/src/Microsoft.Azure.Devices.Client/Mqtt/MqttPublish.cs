using System.Buffers;

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttPublish
    {
        /// <summary>
        ///     Gets or sets the content type.
        ///     The content type must be a UTF-8 encoded string. The content type value identifies the kind of UTF-8 encoded
        ///     payload.
        /// </summary>
        public string? ContentType { get; set; }

        /// <summary>
        ///     Gets or sets the correlation data.
        ///     In order for the sender to know what sent message the response refers to it can also send correlation data with the
        ///     published message.
        ///     Hint: MQTT 5 feature only.
        /// </summary>
        public byte[]? CorrelationData { get; set; }

        /// <summary>
        ///     Gets or sets the message expiry interval.
        ///     A client can set the message expiry interval in seconds for each PUBLISH message individually.
        ///     This interval defines the period of time that the broker stores the PUBLISH message for any matching subscribers
        ///     that are not currently connected.
        ///     When no message expiry interval is set, the broker must store the message for matching subscribers indefinitely.
        ///     When the retained=true option is set on the PUBLISH message, this interval also defines how long a message is
        ///     retained on a topic.
        ///     Hint: MQTT 5 feature only.
        /// </summary>
        public uint MessageExpiryInterval { get; set; }

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

        public ReadOnlySequence<byte> PayloadAsReadOnlySequence { get; set; }

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
        ///     Gets or sets the payload format indicator.
        ///     The payload format indicator is part of any MQTT packet that can contain a payload. The indicator is an optional
        ///     byte value.
        ///     A value of 0 indicates an “unspecified byte stream”.
        ///     A value of 1 indicates a "UTF-8 encoded payload".
        ///     If no payload format indicator is provided, the default value is 0.
        ///     Hint: MQTT 5 feature only.
        /// </summary>
        public MqttPayloadFormatIndicator PayloadFormatIndicator { get; set; }

        /// <summary>
        ///     Gets or sets the quality of service level.
        ///     The Quality of Service (QoS) level is an agreement between the sender of a message and the receiver of a message
        ///     that defines the guarantee of delivery for a specific message.
        ///     There are 3 QoS levels in MQTT:
        ///     - At most once  (0): Message gets delivered no time, once or multiple times.
        ///     - At least once (1): Message gets delivered at least once (one time or more often).
        ///     - Exactly once  (2): Message gets delivered exactly once (It's ensured that the message only comes once).
        /// </summary>
        public MqttQualityOfServiceLevel QualityOfServiceLevel { get; set; }

        /// <summary>
        ///     Gets or sets the MQTT topic.
        ///     In MQTT, the word topic refers to an UTF-8 string that the broker uses to filter messages for each connected
        ///     client.
        ///     The topic consists of one or more topic levels. Each topic level is separated by a forward slash (topic level
        ///     separator).
        /// </summary>
        public required string Topic { get; set; }

        /// <summary>
        ///     Gets or sets the user properties.
        ///     In MQTT 5, user properties are basic UTF-8 string key-value pairs that you can append to almost every type of MQTT
        ///     packet.
        ///     As long as you don’t exceed the maximum message size, you can use an unlimited number of user properties to add
        ///     metadata to MQTT messages and pass information between publisher, broker, and subscriber.
        ///     The feature is very similar to the HTTP header concept.
        ///     Hint: MQTT 5 feature only.
        /// </summary>
        public List<MqttUserProperty> UserProperties { get; set; } = new();

        public void AddUserProperty(string key, string value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }

        /// <summary>
        /// Adds a user property with a pre-encoded UTF-8 byte value.
        /// This overload is more performant when the value is already available as bytes.
        /// </summary>
        /// <param name="key">The property name.</param>
        /// <param name="value">The property value as ReadOnlyMemory of bytes.</param>
        public void AddUserProperty(string key, ReadOnlyMemory<byte> value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }

        /// <summary>
        /// Adds a user property with a pre-encoded UTF-8 byte value.
        /// This overload is more performant when the value is already available as bytes.
        /// </summary>
        /// <param name="key">The property name.</param>
        /// <param name="value">The property value as an ArraySegment of bytes.</param>
        public void AddUserProperty(string key, ArraySegment<byte> value)
        {
            UserProperties ??= [];
            UserProperties.Add(new MqttUserProperty(key, value));
        }
    }
}
