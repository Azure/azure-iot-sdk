using MQTTnet.Protocol;
using MQTTnet.Server;
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
    /// It also implements <see cref="IStubDeviceConnectionDropper"/>, which is what lets a <see cref="StubIotHubService"/>
    /// terminate a device's connection with a chosen MQTT disconnect reason code. Only the broker can do that, since a
    /// second MQTT client has no way to close another client's session.
    /// </para>
    /// </remarks>
    public sealed class InProcessMqttBroker : IAsyncDisposable, IStubDeviceConnectionDropper
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

        /// <summary>
        /// The client ids that currently have a live session on this broker.
        /// </summary>
        /// <remarks>
        /// This includes every client, not just devices: the <see cref="StubIotHubService"/> and the
        /// <see cref="StubDeviceProvisioningService"/> are clients of this broker too.
        /// </remarks>
        public async Task<IReadOnlyList<string>> GetConnectedClientIdsAsync(CancellationToken cancellationToken = default)
        {
            IList<MqttClientStatus> clients = await _server.GetClientsAsync().WaitAsync(cancellationToken).ConfigureAwait(false);

            return [.. clients.Select(client => client.Id)];
        }

        /// <summary>
        /// Terminate a client's connection with the given MQTT disconnect reason code.
        /// </summary>
        /// <remarks>
        /// MQTT 3.1.1 has no server-to-client DISCONNECT packet, so a real classic hub or DPS endpoint can only close the
        /// socket. MQTTnet's broker is more forthcoming and hands the reason code to 3.1.1 clients as well, which means a
        /// gen1 or provisioning device sees a reason here that it would not see in the cloud. Tests that care about the
        /// difference should assert against the stub's own drop history rather than the device's disconnect arguments.
        /// </remarks>
        /// <returns>True if a connection was found and dropped, false if no such client was connected.</returns>
        public async Task<bool> DisconnectClientAsync(
            string clientId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString = null,
            CancellationToken cancellationToken = default)
        {
            ArgumentException.ThrowIfNullOrEmpty(clientId);

            MqttServerClientDisconnectOptionsBuilder optionsBuilder = new MqttServerClientDisconnectOptionsBuilder()
                .WithReasonCode(reasonCode);

            if (reasonString != null)
            {
                optionsBuilder.WithReasonString(reasonString);
            }

            IReadOnlyList<string> connectedClientIds = await GetConnectedClientIdsAsync(cancellationToken).ConfigureAwait(false);
            if (!connectedClientIds.Contains(clientId, StringComparer.Ordinal))
            {
                return false;
            }

            try
            {
                await _server.DisconnectClientAsync(clientId, optionsBuilder.Build()).WaitAsync(cancellationToken).ConfigureAwait(false);
                return true;
            }
            catch (Exception e) when (e is ObjectDisposedException or InvalidOperationException or KeyNotFoundException)
            {
                // The client raced this call and disconnected on its own, or the broker is shutting down.
                return false;
            }
        }

        /// <summary>
        /// The device-oriented view of <see cref="GetConnectedClientIdsAsync(CancellationToken)"/>. The SDK connects a device
        /// with its device id as the MQTT client id, so the two are the same string.
        /// </summary>
        Task<IReadOnlyList<string>> IStubDeviceConnectionDropper.GetConnectedDeviceIdsAsync(CancellationToken cancellationToken)
        {
            return GetConnectedClientIdsAsync(cancellationToken);
        }

        /// <summary>
        /// The device-oriented view of <see cref="DisconnectClientAsync(string, MqttDisconnectReasonCode, string, CancellationToken)"/>.
        /// </summary>
        Task<bool> IStubDeviceConnectionDropper.DropDeviceConnectionAsync(
            string deviceId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString,
            CancellationToken cancellationToken)
        {
            return DisconnectClientAsync(deviceId, reasonCode, reasonString, cancellationToken);
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
