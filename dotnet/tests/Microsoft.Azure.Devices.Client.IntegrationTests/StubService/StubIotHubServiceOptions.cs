using System.Security.Cryptography.X509Certificates;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The configuration for a <see cref="StubIotHubService"/>.
    /// </summary>
    public class StubIotHubServiceOptions
    {
        /// <summary>
        /// Which generation of IoT hub this stub should behave like.
        /// </summary>
        /// <remarks>
        /// <see cref="IotHubGeneration.Gen1"/> makes the stub connect to the broker with MQTT 3.1.1 and speak the classic
        /// <c>$iothub/...</c> protocol. <see cref="IotHubGeneration.Gen2"/> makes the stub connect with MQTT 5 and speak the
        /// Azure Event Grid <c>ih/...</c> protocol.
        /// </remarks>
        public IotHubGeneration Generation { get; set; } = IotHubGeneration.Gen2;

        /// <summary>
        /// The host name this stub hub presents itself as.
        /// </summary>
        /// <remarks>
        /// The stub does not listen on this host name - it reaches devices through <see cref="BrokerHostName"/>. This value is
        /// what a <see cref="StubDeviceProvisioningService"/> hands back to a device as its assigned hub, and what a test would
        /// put in <see cref="Models.ConnectionContext.IotHubHostName"/>.
        /// </remarks>
        public string HubHostName { get; set; } = "stub-iot-hub.azure-devices.net";

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
        public string ClientId { get; set; } = "stub-iot-hub-service-" + Guid.NewGuid().ToString("N");

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
        /// Restrict this stub to a single device id. When null, the stub serves every device id it sees traffic from, which
        /// mirrors how a real hub behaves.
        /// </summary>
        public string? DeviceIdFilter { get; set; }

        /// <summary>
        /// Whether the stub honors the <c>push_desired</c> / <c>push_reported</c> bits on a device's birth message by publishing
        /// a birth-triggered twin push. Only applicable to <see cref="IotHubGeneration.Gen2"/>.
        /// </summary>
        /// <remarks>
        /// Setting this to false lets a test exercise the "pull-only" mode described in the twin design doc section 4.3 even when
        /// the device requests a push.
        /// </remarks>
        public bool EnableBirthTriggeredTwinPush { get; set; } = true;

        /// <summary>
        /// How long the stub waits for a device to acknowledge a direct method delivery probe. This becomes the probe's MQTT 5
        /// message expiry interval. Only applicable to <see cref="IotHubGeneration.Gen2"/>.
        /// </summary>
        public TimeSpan DefaultDirectMethodConnectTimeout { get; set; } = TimeSpan.FromSeconds(30);

        /// <summary>
        /// How long the stub waits for a device to return a direct method result. This becomes the exec message's MQTT 5 message
        /// expiry interval for gen2, and the overall wait for a response for gen1.
        /// </summary>
        public TimeSpan DefaultDirectMethodResponseTimeout { get; set; } = TimeSpan.FromSeconds(30);

        /// <summary>
        /// An optional sink for the stub's diagnostic messages.
        /// </summary>
        public Action<string>? Logger { get; set; }

        /// <summary>
        /// How the stub terminates a device's MQTT connection.
        /// </summary>
        /// <remarks>
        /// The stub is an MQTT client, not a broker, so it cannot close another client's session by itself. Supplying a
        /// dropper - <see cref="MqttFaultInjectionClient"/>, which asks an <see cref="InProcessMqttBroker"/> for the drop
        /// over MQTT, is one - enables <see cref="StubIotHubService.DropDeviceConnectionAsync"/> and the
        /// <see cref="RandomConnectionDrops"/> loop. When null, any attempt to drop a connection throws.
        /// </remarks>
        public IStubDeviceConnectionDropper? ConnectionDropper { get; set; }

        /// <summary>
        /// How, and whether, the stub randomly drops the connections of the devices it serves, with randomly chosen MQTT
        /// disconnect reason codes.
        /// </summary>
        /// <remarks>
        /// Random drops are off until <see cref="StubConnectionDropOptions.Enabled"/> is set, and require
        /// <see cref="ConnectionDropper"/> to be set as well.
        /// </remarks>
        public StubConnectionDropOptions RandomConnectionDrops { get; set; } = new();
    }
}
