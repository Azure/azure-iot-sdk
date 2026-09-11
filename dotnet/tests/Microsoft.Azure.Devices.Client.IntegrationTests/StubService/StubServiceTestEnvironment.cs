using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Retry;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// An entire IoT solution running in the test process: an MQTT broker, a <see cref="StubIotHubService"/>, and a
    /// <see cref="StubDeviceProvisioningService"/> bound to it. No cloud resources are involved.
    /// </summary>
    /// <remarks>
    /// This exists so that a test can provision and connect a real device client in a handful of lines. The device
    /// side of the wiring is deliberately left to the test, since which connection client to use is usually the thing
    /// under test.
    /// </remarks>
    public sealed class StubServiceTestEnvironment : IAsyncDisposable
    {
        /// <summary>
        /// The host name the stub DPS assigns devices to, and that the stub hub answers for.
        /// </summary>
        public const string HubHostName = "stub-hub.azure-devices.net";

        /// <summary>
        /// The id scope devices provision against. The stub DPS accepts any id scope.
        /// </summary>
        public const string IdScope = "0ne00000000";

        private InProcessMqttBroker _broker = null!;
        private X509Certificate2 _certificate = null!;

        private StubServiceTestEnvironment()
        {
        }

        /// <summary>
        /// The single device this environment is set up to serve.
        /// </summary>
        public string DeviceId { get; } = "stub-device-" + Guid.NewGuid().ToString("N")[..8];

        /// <summary>
        /// The hub the device will be assigned to.
        /// </summary>
        public StubIotHubService Hub { get; private set; } = null!;

        /// <summary>
        /// The DPS that assigns the device to <see cref="Hub"/>.
        /// </summary>
        public StubDeviceProvisioningService ProvisioningService { get; private set; } = null!;

        /// <summary>
        /// Start a broker, a hub of the requested generation, and a DPS that assigns devices to that hub.
        /// </summary>
        public static async Task<StubServiceTestEnvironment> StartAsync(IotHubGeneration generation, CancellationToken cancellationToken = default)
        {
            var environment = new StubServiceTestEnvironment();

            try
            {
                environment._broker = await InProcessMqttBroker.StartAsync(cancellationToken);
                environment._certificate = CreateSelfSignedCertificate(environment.DeviceId);

                environment.Hub = new StubIotHubService(new StubIotHubServiceOptions
                {
                    Generation = generation,
                    HubHostName = HubHostName,
                    BrokerHostName = environment._broker.HostName,
                    BrokerPort = environment._broker.Port,

                    // Classic hub topics carry no device id, so the stub has to be told which device it is serving.
                    DeviceIdFilter = generation == IotHubGeneration.Gen1 ? environment.DeviceId : null,
                });

                await environment.Hub.StartAsync(cancellationToken);

                // ForHub copies the hub's generation, host name, and broker endpoint, so the device is guaranteed to
                // be told to speak the protocol the hub is actually serving.
                environment.ProvisioningService = StubDeviceProvisioningService.ForHub(
                    environment.Hub,
                    options => options.DeviceId = environment.DeviceId);

                await environment.ProvisioningService.StartAsync(cancellationToken);

                return environment;
            }
            catch
            {
                await environment.DisposeAsync();
                throw;
            }
        }

        /// <summary>
        /// The options to hand a device connection client so that it talks to this environment.
        /// </summary>
        /// <remarks>
        /// Only the CONNECT packet is redirected. Every topic, payload, and correlation above it is the unmodified
        /// SDK code path.
        /// </remarks>
        public ConnectionClientOptions CreateConnectionClientOptions()
        {
            return new ConnectionClientOptions
            {
                MqttClient = new LocalBrokerMqttClient(_broker.HostName, _broker.Port),

                // Without this, a broker that drops the connection mid-test would keep the test hanging in retries.
                ConnectionRetryPolicy = new NoRetry(),
            };
        }

        /// <summary>
        /// The authentication the device provisions and connects with. The certificate's common name is
        /// <see cref="DeviceId"/>, which is what the SDK derives the DPS registration id from.
        /// </summary>
        public X509AuthenticationProvider CreateAuthenticationProvider()
        {
            return new X509AuthenticationProvider(_certificate);
        }

        /// <summary>
        /// The settings the device provisions with.
        /// </summary>
        public ProvisioningSettings CreateProvisioningSettings()
        {
            return new ProvisioningSettings(IdScope);
        }

        public async ValueTask DisposeAsync()
        {
            if (ProvisioningService != null)
            {
                await ProvisioningService.DisposeAsync();
            }

            if (Hub != null)
            {
                await Hub.DisposeAsync();
            }

            if (_broker != null)
            {
                await _broker.DisposeAsync();
            }

            _certificate?.Dispose();
        }

        /// <summary>
        /// The SDK requires a client certificate to build its CONNECT packet and to derive the registration id, but
        /// <see cref="LocalBrokerMqttClient"/> strips it before connecting to the plaintext local broker.
        /// </summary>
        private static X509Certificate2 CreateSelfSignedCertificate(string deviceId)
        {
            using RSA key = RSA.Create(2048);

            var request = new CertificateRequest($"CN={deviceId}", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

            return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
        }
    }
}
