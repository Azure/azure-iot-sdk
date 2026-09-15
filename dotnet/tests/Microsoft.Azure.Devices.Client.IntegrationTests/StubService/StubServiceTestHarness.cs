using Microsoft.Azure.Devices.Client.Models;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;
using Gen2ConnectionClient = Microsoft.Azure.Devices.Client.Gen2.Connection.ConnectionClient;
using UnifiedConnectionClient = Microsoft.Azure.Devices.Client.Unified.Connection.ConnectionClient;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// Wires an <see cref="InProcessMqttBroker"/>, a <see cref="StubIotHubService"/>, a
    /// <see cref="StubDeviceProvisioningService"/>, and a device client together.
    /// </summary>
    /// <remarks>
    /// This is the harness the stub's own tests use. It is deliberately opinionated: one device, one broker, one hub, and
    /// a DPS bound to that hub, because every device reaches the stub hub by being provisioned onto it.
    /// </remarks>
    internal sealed class StubServiceTestHarness : IAsyncDisposable
    {
        public const string StubHostName = "stub.azure-devices.net";
        public const string IdScope = "0ne00000000";

        private readonly List<IDisposable> _disposables = new();
        private InProcessMqttBroker _broker = null!;
        private X509Certificate2 _certificate = null!;
        private Action<ConnectionClientOptions>? _configureClientOptions;

        public string DeviceId { get; } = "stub-device-" + Guid.NewGuid().ToString("N")[..8];

        public StubIotHubService Stub { get; private set; } = null!;

        /// <summary>
        /// The stub DPS, if this harness was started with one.
        /// </summary>
        public StubDeviceProvisioningService Dps { get; private set; } = null!;

        /// <summary>
        /// The broker everything in this harness is attached to, and what backs the stub hub's connection drops.
        /// </summary>
        public InProcessMqttBroker Broker => _broker;

        /// <summary>
        /// The MQTT client the most recently built device client connects with. Tests that care about MQTT level
        /// events, such as the DISCONNECT packet the stub hub causes the broker to send, observe them here.
        /// </summary>
        public LocalBrokerMqttClient DeviceMqttClient { get; private set; } = null!;

        /// <param name="generation">Which generation of hub the stub should serve, and assign the device to.</param>
        /// <param name="configureProvisioningService">Applied to the DPS options after this harness has set its own.</param>
        /// <param name="configureHub">Applied to the hub options after this harness has set its own.</param>
        /// <param name="configureClientOptions">
        /// Applied to every device client this harness builds, after its own defaults. This is how a test replaces the
        /// default <see cref="Retry.NoRetry"/> policy, which exists so that a dropped connection fails the test's
        /// operation instead of hanging it in retries.
        /// </param>
        public static async Task<StubServiceTestHarness> StartAsync(
            IotHubGeneration generation,
            Action<StubDeviceProvisioningServiceOptions>? configureProvisioningService = null,
            Action<StubIotHubServiceOptions>? configureHub = null,
            Action<ConnectionClientOptions>? configureClientOptions = null)
        {
            var harness = new StubServiceTestHarness
            {
                _configureClientOptions = configureClientOptions,
            };

            harness._broker = await InProcessMqttBroker.StartAsync(TestContext.Current.CancellationToken);
            harness._certificate = CreateSelfSignedCertificate(harness.DeviceId);

            var hubOptions = new StubIotHubServiceOptions
            {
                Generation = generation,
                HubHostName = StubHostName,
                BrokerHostName = harness._broker.HostName,
                BrokerPort = harness._broker.Port,

                // Classic hub topics carry no device id, so the stub has to be told which device it is serving.
                DeviceIdFilter = generation == IotHubGeneration.Gen1 ? harness.DeviceId : null,

                // Only the broker can close a device's session, so it is what backs the hub's connection drops.
                ConnectionDropper = harness._broker,
            };

            configureHub?.Invoke(hubOptions);

            harness.Stub = new StubIotHubService(hubOptions);

            await harness.Stub.StartAsync(TestContext.Current.CancellationToken);

            // Devices only ever reach the hub by being provisioned onto it, so the DPS is always part of the harness.
            harness.Dps = StubDeviceProvisioningService.ForHub(harness.Stub, options =>
            {
                options.DeviceId = harness.DeviceId;
                configureProvisioningService?.Invoke(options);
            });

            await harness.Dps.StartAsync(TestContext.Current.CancellationToken);

            return harness;
        }

        /// <summary>
        /// Provisions the device through the stub DPS and connects it to the gen2 hub it is assigned.
        /// </summary>
        public async Task<Gen2ConnectionClient> ConnectGen2DeviceAsync()
        {
            var connectionClient = new Gen2ConnectionClient(BuildClientOptions());
            _disposables.Add(connectionClient);

            await connectionClient.ProvisionAndConnectAsync(
                new ProvisioningSettings(IdScope),
                new X509AuthenticationProvider(_certificate),
                TestContext.Current.CancellationToken);

            return connectionClient;
        }

        /// <summary>
        /// Provisions the device through the stub DPS and connects it to the gen1 hub it is assigned.
        /// </summary>
        public async Task<UnifiedConnectionClient> ConnectGen1DeviceAsync()
        {
            (UnifiedConnectionClient client, _) = await ProvisionAndConnectDeviceAsync();

            return client;
        }

        /// <summary>
        /// Runs the SDK's full provisioning flow on the unified client: register with the stub DPS, then connect to
        /// whichever hub, and whichever generation of hub, it assigned.
        /// </summary>
        public async Task<(UnifiedConnectionClient Client, ConnectionContext Context)> ProvisionAndConnectDeviceAsync()
        {
            var connectionClient = new UnifiedConnectionClient(BuildClientOptions());
            _disposables.Add(connectionClient);

            ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
                new ProvisioningSettings(IdScope),
                new X509AuthenticationProvider(_certificate),
                TestContext.Current.CancellationToken);

            return (connectionClient, context);
        }

        public async ValueTask DisposeAsync()
        {
            foreach (IDisposable disposable in _disposables)
            {
                try
                {
                    disposable.Dispose();
                }
                catch (Exception)
                {
                    // Teardown of a test harness should never mask the test's own failure.
                }
            }

            if (Stub != null)
            {
                await Stub.DisposeAsync();
            }

            if (Dps != null)
            {
                await Dps.DisposeAsync();
            }

            if (_broker != null)
            {
                await _broker.DisposeAsync();
            }

            _certificate?.Dispose();
        }

        private ConnectionClientOptions BuildClientOptions()
        {
            DeviceMqttClient = new LocalBrokerMqttClient(_broker.HostName, _broker.Port);

            var options = new ConnectionClientOptions
            {
                MqttClient = DeviceMqttClient,

                // Without this, a broker that drops the connection mid-test would keep the test hanging in retries.
                ConnectionRetryPolicy = new Retry.NoRetry(),
            };

            _configureClientOptions?.Invoke(options);

            return options;
        }

        /// <summary>
        /// The SDK requires a client certificate to build its CONNECT packet, and derives the DPS registration id
        /// from its common name, but <see cref="LocalBrokerMqttClient"/> strips it before connecting to the
        /// plaintext local broker.
        /// </summary>
        private static X509Certificate2 CreateSelfSignedCertificate(string deviceId)
        {
            using RSA key = RSA.Create(2048);

            var request = new CertificateRequest($"CN={deviceId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

            return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }
    }
}
