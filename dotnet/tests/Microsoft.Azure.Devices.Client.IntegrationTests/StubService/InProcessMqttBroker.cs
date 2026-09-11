using MQTTnet.Server;
using System.Net;
using System.Net.Sockets;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// A plaintext MQTT broker that runs in the test process on a loopback port.
    /// </summary>
    /// <remarks>
    /// Neither a real IoT hub nor the <see cref="StubIotHubService"/> is a broker. The gen2 IoT hub is fronted by the
    /// Azure Event Grid MQTT broker, and the gen1 IoT hub embeds its own. This class stands in for that broker so that
    /// a device client and the stub service can be pointed at the same endpoint without any cloud resources.
    /// </remarks>
    public sealed class InProcessMqttBroker : IAsyncDisposable
    {
        private readonly MqttServer _server;

        private InProcessMqttBroker(MqttServer server, int port)
        {
            _server = server;
            Port = port;
        }

        /// <summary>
        /// The loopback host name that both the device client and the stub service should connect to.
        /// </summary>
        public string HostName => "127.0.0.1";

        /// <summary>
        /// The TCP port that this broker listens on.
        /// </summary>
        public int Port { get; }

        /// <summary>
        /// Start a broker on an unused loopback port.
        /// </summary>
        public static async Task<InProcessMqttBroker> StartAsync(CancellationToken cancellationToken = default)
        {
            int port = GetFreeTcpPort();

            MqttServerOptions options = new MqttServerOptionsBuilder()
                .WithDefaultEndpoint()
                .WithDefaultEndpointBoundIPAddress(IPAddress.Loopback)
                .WithDefaultEndpointPort(port)
                .Build();

            MqttServer server = new MqttServerFactory().CreateMqttServer(options);

            // This broker exists only to move packets between a device client and the stub service, so every connection
            // attempt is accepted. Authentication and authorization are the responsibility of the real broker.
            server.ValidatingConnectionAsync += args =>
            {
                args.ReasonCode = MQTTnet.Protocol.MqttConnectReasonCode.Success;
                return Task.CompletedTask;
            };

            await server.StartAsync().WaitAsync(cancellationToken).ConfigureAwait(false);

            return new InProcessMqttBroker(server, port);
        }

        public async ValueTask DisposeAsync()
        {
            try
            {
                await _server.StopAsync().ConfigureAwait(false);
            }
            catch (ObjectDisposedException)
            {
                // The server was already stopped.
            }

            _server.Dispose();
        }

        private static int GetFreeTcpPort()
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();

            try
            {
                return ((IPEndPoint)listener.LocalEndpoint).Port;
            }
            finally
            {
                listener.Stop();
            }
        }
    }
}
