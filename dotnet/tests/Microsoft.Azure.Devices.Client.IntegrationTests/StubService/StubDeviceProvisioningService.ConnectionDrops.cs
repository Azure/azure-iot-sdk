using MQTTnet.Protocol;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The half of the stub DPS that terminates the registering device's connection, which is how a test reproduces a
    /// registration that is interrupted part way through.
    /// </summary>
    /// <remarks>
    /// DPS cannot persist sessions, so the SDK treats any connection loss here as the loss of the in-progress registration
    /// and restarts the flow from the beginning once it reconnects. Dropping the connection at an arbitrary moment is the
    /// only way to exercise that path.
    /// </remarks>
    public sealed partial class StubDeviceProvisioningService
    {
        private StubConnectionDropEngine? _connectionDrops;

        /// <summary>
        /// Raised after the stub drops the registering device's connection, whether the drop was explicit or came from the
        /// random drop loop.
        /// </summary>
        public event EventHandler<StubDeviceConnectionDroppedEventArgs>? DeviceConnectionDropped;

        /// <summary>
        /// Whether this stub can drop device connections, which it can only do when an
        /// <see cref="StubDeviceProvisioningServiceOptions.ConnectionDropper"/> was supplied.
        /// </summary>
        public bool CanDropConnections => ConnectionDrops.CanDropConnections;

        /// <summary>
        /// Every drop this stub has performed, oldest first.
        /// </summary>
        /// <remarks>
        /// A randomized test asserts against this, since neither the moment nor the reason code is known up front.
        /// </remarks>
        public IReadOnlyList<StubDeviceConnectionDroppedEventArgs> ConnectionDropHistory => ConnectionDrops.History;

        private StubConnectionDropEngine ConnectionDrops => _connectionDrops ??= new StubConnectionDropEngine(
            _options.RandomConnectionDrops,
            _options.ConnectionDropper,
            canDropDevice: IsRegisteringDevice,
            isRandomDropCandidate: IsRegisteringDevice,
            onDropped: args => DeviceConnectionDropped?.Invoke(this, args),
            log: Log,
            ownerDescription: "stub DPS",
            dropperOptionName: $"{nameof(StubDeviceProvisioningServiceOptions)}.{nameof(StubDeviceProvisioningServiceOptions.ConnectionDropper)}");

        /// <summary>
        /// Drop the registering device's connection with a specific MQTT disconnect reason code.
        /// </summary>
        /// <param name="deviceId">
        /// The device whose connection should be dropped. This must be the device this stub is configured to register,
        /// because the SDK connects to DPS with the registration id as its MQTT client id.
        /// </param>
        /// <param name="reasonCode">The reason code to send.</param>
        /// <param name="reasonString">
        /// The optional reason string to accompany the reason code. Defaults to
        /// <see cref="StubConnectionDropOptions.ReasonString"/>.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>True if the device was connected and its connection was dropped, false if it was not connected.</returns>
        /// <remarks>
        /// Provisioning is MQTT 3.1.1 for both generations, and MQTT 3.1.1 has no server-to-client DISCONNECT packet, so a
        /// real DPS endpoint could only close the socket. <see cref="InProcessMqttBroker"/> does send the reason code to
        /// 3.1.1 clients, so the device will see one here that it would not see in the cloud. The code is recorded either
        /// way, which is what a test should assert on.
        /// </remarks>
        public async Task<bool> DropDeviceConnectionAsync(
            string deviceId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString = null,
            CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentException.ThrowIfNullOrEmpty(deviceId);

            return await ConnectionDrops.DropAsync(deviceId, reasonCode, reasonString, wasRandom: false, cancellationToken) != null;
        }

        /// <summary>
        /// Drop the registering device's connection with a reason code drawn at random from
        /// <see cref="StubConnectionDropOptions.ReasonCodes"/>.
        /// </summary>
        /// <param name="deviceId">The device whose connection should be dropped.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The reason code that was chosen, or null if the device was not connected.</returns>
        public async Task<MqttDisconnectReasonCode?> DropDeviceConnectionWithRandomReasonCodeAsync(
            string deviceId,
            CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentException.ThrowIfNullOrEmpty(deviceId);

            StubDeviceConnectionDroppedEventArgs? dropped = await ConnectionDrops.DropAsync(
                deviceId,
                ConnectionDrops.NextRandomReasonCode(),
                reasonString: null,
                wasRandom: true,
                cancellationToken);

            return dropped?.ReasonCode;
        }

        /// <summary>
        /// Drop the registering device, if it is connected, with a randomly chosen reason code.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>What was dropped, or null if the device this stub registers was not connected.</returns>
        public async Task<StubDeviceConnectionDroppedEventArgs?> DropRandomDeviceConnectionAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            return await ConnectionDrops.DropRandomAsync(cancellationToken);
        }

        /// <summary>
        /// Pick a reason code at random from <see cref="StubConnectionDropOptions.ReasonCodes"/>.
        /// </summary>
        public MqttDisconnectReasonCode NextRandomReasonCode()
        {
            return ConnectionDrops.NextRandomReasonCode();
        }

        /// <summary>
        /// Whether the given MQTT client id is the device this stub registers.
        /// </summary>
        /// <remarks>
        /// The SDK connects to DPS with the registration id as the client id, and a stub sharing a broker cannot see another
        /// client's CONNECT packet, so the configured ids are the only devices the stub can recognize. Everything else on
        /// the broker - the stub hub, this stub, other tests' clients - is off limits.
        /// </remarks>
        private bool IsRegisteringDevice(string deviceId)
        {
            return string.Equals(deviceId, _options.DeviceId, StringComparison.Ordinal)
                || string.Equals(deviceId, _options.RegistrationId, StringComparison.Ordinal);
        }
    }
}
