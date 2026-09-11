using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// An MQTT client that retargets every CONNECT packet the SDK builds at a plaintext local broker.
    /// </summary>
    /// <remarks>
    /// The SDK always builds its CONNECT packet for a real IoT hub: TLS on port 8883 with the device's client
    /// certificate. Supplying an instance of this class through <see cref="Models.ConnectionClientOptions.MqttClient"/>
    /// rewrites the host, the port, and drops the client certificate so that the otherwise untouched SDK client talks
    /// to an <see cref="InProcessMqttBroker"/> instead. Everything above the CONNECT packet - topics, payloads,
    /// correlation, subscriptions - is the real SDK code path.
    /// </remarks>
    public sealed class LocalBrokerMqttClient : MqttNetClient
    {
        private readonly string _hostName;
        private readonly int _port;

        public LocalBrokerMqttClient(string hostName, int port, bool enableMqttLogs = false)
            : base(enableMqttLogs: enableMqttLogs)
        {
            _hostName = hostName;
            _port = port;
        }

        public override Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
            connect.HostName = _hostName;
            connect.TcpPort = _port;
            connect.WebsocketUri = null;
            connect.UseTls = false;

            // The adapter only negotiates TLS when a client certificate is present, and the local broker has no TLS
            // endpoint. The device id still reaches the stub service by way of the client id and the gen2 topics.
            connect.ClientCertificate = null;

            return base.ConnectAsync(connect, cancellationToken);
        }
    }
}
