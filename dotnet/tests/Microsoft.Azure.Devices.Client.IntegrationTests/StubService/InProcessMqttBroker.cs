using MQTTnet.Server;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// A plaintext MQTT broker that runs in the test process on a loopback port.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Neither a real IoT hub nor the <see cref="StubIotHubService"/> is a broker. The gen2 IoT hub is fronted by the
    /// Azure Event Grid MQTT broker, and the gen1 IoT hub embeds its own. This class stands in for that broker so that
    /// a device client and the stub service can be pointed at the same endpoint without any cloud resources.
    /// </para>
    /// <para>
    /// It is also where fault injection happens, since only the broker can terminate a client's session. Every fault it
    /// can inject is triggered by a PUBLISH to <see cref="MqttFaultInjection.RequestTopic"/> - there is no .NET method
    /// here that injects one - so a fault can be triggered from outside this process. <see cref="MqttFaultInjectionClient"/>
    /// is the in-process way to send those publishes, and it is what backs both stubs' connection drops.
    /// </para>
    /// </remarks>
    public sealed partial class InProcessMqttBroker : IAsyncDisposable
    {
        private readonly MqttServer _server;
        private readonly Action<string>? _logger;

        private InProcessMqttBroker(MqttServer server, int port, Action<string>? logger)
        {
            _server = server;
            Port = port;
            _logger = logger;
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
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <param name="logger">An optional sink for the broker's diagnostic messages, such as the faults it injects.</param>
        public static async Task<InProcessMqttBroker> StartAsync(CancellationToken cancellationToken = default, Action<string>? logger = null)
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

            var broker = new InProcessMqttBroker(server, port, logger);
            broker.AttachFaultInjection();

            await server.StartAsync().WaitAsync(cancellationToken).ConfigureAwait(false);

            return broker;
        }

        /// <summary>
        /// The client ids that currently have a live session on this broker.
        /// </summary>
        /// <remarks>
        /// This includes every client, not just devices: the <see cref="StubIotHubService"/>, the
        /// <see cref="StubDeviceProvisioningService"/> and any <see cref="MqttFaultInjectionClient"/> are clients of this
        /// broker too. Callers outside this process reach the same list through the
        /// <see cref="MqttFaultInjection.Faults.ListClients"/> request.
        /// </remarks>
        public async Task<IReadOnlyList<string>> GetConnectedClientIdsAsync(CancellationToken cancellationToken = default)
        {
            IList<MqttClientStatus> clients = await _server.GetClientsAsync().WaitAsync(cancellationToken).ConfigureAwait(false);

            return [.. clients.Select(client => client.Id)];
        }

        public async ValueTask DisposeAsync()
        {
            await _faultInjectionCancellation.CancelAsync();
            DetachFaultInjection();

            try
            {
                await _server.StopAsync().ConfigureAwait(false);
            }
            catch (ObjectDisposedException)
            {
                // The server was already stopped.
            }

            _server.Dispose();
            _faultInjectionCancellation.Dispose();
        }

        private void Log(string message)
        {
            _logger?.Invoke(message);
            Trace.TraceInformation(message);
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
