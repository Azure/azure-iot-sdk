// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Net.Security;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public class MqttConnect
    {
        /// <summary>
        ///     Gets or sets the authentication data.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public byte[]? AuthenticationData { get; set; }

        /// <summary>
        ///     Gets or sets the authentication method.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public string? AuthenticationMethod { get; set; }

        // TODO document that this is for MQTT v3.1.1 only!
        public bool CleanSession { get; set; }

        // TODO document that this is for MQTT v5 only! MQTTnet combined this + clean session into just one field "cleanSession" which is maybe the correct approach
        public bool CleanStart { get; set; }

        public X509Certificate2? ClientCertificate { get; set; }

        /// <summary>
        ///     An optional callback for custom validation of the server (remote) certificate during the TLS handshake.
        ///     When null, the platform's default validation is used.
        /// </summary>
        public RemoteCertificateValidationCallback? RemoteCertificateValidationCallback { get; set; }

        /// <summary>
        ///     An optional callback for selecting which client certificate to present during the TLS handshake.
        ///     When null, <see cref="ClientCertificate"/> is presented.
        /// </summary>
        public LocalCertificateSelectionCallback? LocalCertificateSelectionCallback { get; set; }

        /// <summary>
        ///     Gets the client identifier.
        ///     Hint: This identifier needs to be unique over all used clients / devices on the broker to avoid connection issues.
        /// </summary>
        public string? ClientId { get; set; }

        public string? Username { get; set; }

        public byte[]? Password { get; set; } // Should always be empty array in x509 only world

        public required string HostName { get; set; }

        public int TcpPort { get; set; }

        public string? WebsocketUri { get; set; }

        public int WebsocketPort { get; set; }

        public bool UseTls = true; // IoT hub + DPS connections will always use TLS

        /// <summary>
        ///     Gets or sets the keep alive period.
        ///     The connection is normally left open by the client so that is can send and receive data at any time.
        ///     If no data flows over an open connection for a certain time period then the client will generate a PINGREQ and
        ///     expect to receive a PINGRESP from the broker.
        ///     This message exchange confirms that the connection is open and working.
        ///     This period is known as the keep alive period.
        /// </summary>
        public TimeSpan KeepAlivePeriod { get; set; }

        /// <summary>
        ///     Gets or sets the maximum packet size.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public uint MaximumPacketSize { get; set; }

        /// <summary>
        ///     Gets or sets the protocol version.
        ///     Default: 5.0.0.
        /// </summary>
        public MqttProtocolVersion ProtocolVersion { get; set; }

        /// <summary>
        ///     Gets or sets the receive maximum.
        ///     This gives the maximum length of the received messages.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public ushort ReceiveMaximum { get; set; }

        /// <summary>
        ///     Gets or sets the request problem information.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public bool RequestProblemInformation { get; set; }

        /// <summary>
        ///     Gets or sets the request response information.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public bool RequestResponseInformation { get; set; }

        /// <summary>
        ///     Gets or sets the session expiry interval.
        ///     The time after a session expires when it's not actively used.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public uint SessionExpiryInterval { get; set; }

        /// <summary>
        ///     Gets or sets the user properties.
        ///     In MQTT 5, user properties are basic UTF-8 string key-value pairs that you can append to almost every type of MQTT
        ///     packet.
        ///     As long as you don’t exceed the maximum message size, you can use an unlimited number of user properties to add
        ///     metadata to MQTT messages and pass information between publisher, broker, and subscriber.
        ///     The feature is very similar to the HTTP header concept.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
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
