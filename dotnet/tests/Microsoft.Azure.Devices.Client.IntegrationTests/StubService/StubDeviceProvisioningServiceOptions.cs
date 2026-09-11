using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The configuration for a <see cref="StubDeviceProvisioningService"/>.
    /// </summary>
    public class StubDeviceProvisioningServiceOptions
    {
        /// <summary>
        /// Which generation of IoT hub this stub provisions devices to.
        /// </summary>
        /// <remarks>
        /// This is the switch the whole stub exists for. It is reported to the device as the registration result's
        /// <see cref="DeviceRegistrationResult.ConnectionProfile"/> - <see cref="ConnectionProfile.Classic"/> for gen1
        /// and <see cref="ConnectionProfile.MqttV5"/> for gen2 - which is what the SDK's
        /// <c>ProvisionAndConnectAsync</c> reads to decide whether to follow the classic MQTT 3.1.1 hub path or the gen2
        /// MQTT 5 path. Point this at the same generation as the <see cref="StubIotHubService"/> the device will land on.
        /// </remarks>
        public IotHubGeneration Generation { get; set; } = IotHubGeneration.Gen2;

        /// <summary>
        /// The host name of the MQTT broker that both this stub and the devices under test connect to.
        /// </summary>
        public string BrokerHostName { get; set; } = "localhost";

        /// <summary>
        /// The port of the MQTT broker that both this stub and the devices under test connect to.
        /// </summary>
        public int BrokerPort { get; set; } = 1883;

        /// <summary>
        /// Whether to connect to the broker over TLS.
        /// </summary>
        public bool UseTls { get; set; }

        /// <summary>
        /// Whether to accept broker server certificates that do not chain to a trusted root. Only honored when
        /// <see cref="UseTls"/> is true, and only intended for tests against a broker with a self-signed certificate.
        /// </summary>
        public bool AllowUntrustedCertificates { get; set; }

        /// <summary>
        /// The MQTT client id this stub connects with. Must not collide with any device's client id.
        /// </summary>
        public string ClientId { get; set; } = "stub-dps-service-" + Guid.NewGuid().ToString("N");

        /// <summary>
        /// The optional MQTT username to connect with.
        /// </summary>
        public string? Username { get; set; }

        /// <summary>
        /// The optional MQTT password to connect with.
        /// </summary>
        public string? Password { get; set; }

        /// <summary>
        /// The optional client certificate to authenticate to the broker with.
        /// </summary>
        public X509Certificate2? ClientCertificate { get; set; }

        /// <summary>
        /// The hub host name handed back to the device as its assigned hub.
        /// </summary>
        /// <remarks>
        /// Set this to the <see cref="StubIotHubService.HubHostName"/> of the stub hub the device should land on.
        /// </remarks>
        public string AssignedHubHostName { get; set; } = "stub-iot-hub.azure-devices.net";

        /// <summary>
        /// The device id this stub assigns.
        /// </summary>
        /// <remarks>
        /// A real DPS derives this from the enrollment record keyed on the registration id in the device's CONNECT username.
        /// A stub attached to a shared broker cannot see another client's CONNECT packet, so the device id has to be
        /// configured. When null, <see cref="RegistrationId"/> is used.
        /// </remarks>
        public string? DeviceId { get; set; }

        /// <summary>
        /// The registration id echoed back in the registration result. When null, <see cref="DeviceId"/> is used.
        /// </summary>
        public string? RegistrationId { get; set; }

        /// <summary>
        /// The substatus reported on a successful assignment.
        /// </summary>
        public ProvisioningRegistrationSubstatus Substatus { get; set; } = ProvisioningRegistrationSubstatus.InitialAssignment;

        /// <summary>
        /// How many status polls are answered with <c>assigning</c> before the stub reports <c>assigned</c>.
        /// </summary>
        /// <remarks>
        /// Zero, the default, assigns on the very first poll and keeps tests fast. A larger value exercises the SDK's polling
        /// loop, at the cost of at least <see cref="RetryAfter"/> per extra poll.
        /// </remarks>
        public int AssigningPollResponses { get; set; }

        /// <summary>
        /// The value advertised in the <c>retry-after</c> query parameter of the stub's response topics.
        /// </summary>
        /// <remarks>
        /// The SDK clamps this to a two second floor, so smaller values do not make polling faster.
        /// </remarks>
        public TimeSpan RetryAfter { get; set; } = TimeSpan.FromSeconds(2);

        /// <summary>
        /// An optional sink for the stub's diagnostic messages.
        /// </summary>
        public Action<string>? Logger { get; set; }
    }
}
