using MQTTnet.Protocol;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The half of the stub hub that terminates device connections, which is how a test reproduces the unannounced
    /// disconnects a real service inflicts on a device.
    /// </summary>
    public sealed partial class StubIotHubService
    {
        private StubConnectionDropEngine? _connectionDrops;

        /// <summary>
        /// Raised after the stub drops a device's connection, whether the drop was explicit or came from the random drop
        /// loop.
        /// </summary>
        public event EventHandler<StubDeviceConnectionDroppedEventArgs>? DeviceConnectionDropped;

        /// <summary>
        /// Whether this stub can drop device connections, which it can only do when an
        /// <see cref="StubIotHubServiceOptions.ConnectionDropper"/> was supplied.
        /// </summary>
        public bool CanDropConnections => ConnectionDrops.CanDropConnections;

        /// <summary>
        /// Every drop this stub has performed, oldest first.
        /// </summary>
        /// <remarks>
        /// A randomized test can assert against this instead of against a single drop, since neither the victim, the moment,
        /// nor the reason code is known up front.
        /// </remarks>
        public IReadOnlyList<StubDeviceConnectionDroppedEventArgs> ConnectionDropHistory => ConnectionDrops.History;

        private StubConnectionDropEngine ConnectionDrops => _connectionDrops ??= new StubConnectionDropEngine(
            _options.RandomConnectionDrops,
            _options.ConnectionDropper,
            canDropDevice: IsServedDevice,

            // A device is only a random victim once the stub knows about it, which keeps the stub's own connection and the
            // stub DPS's out of the draw.
            isRandomDropCandidate: deviceId => IsServedDevice(deviceId) && _devices.ContainsKey(deviceId),
            onDropped: HandleConnectionDropped,
            log: Log,
            ownerDescription: $"stub {_options.Generation} IoT hub service",
            dropperOptionName: $"{nameof(StubIotHubServiceOptions)}.{nameof(StubIotHubServiceOptions.ConnectionDropper)}");

        /// <summary>
        /// Drop a device's connection with a specific MQTT disconnect reason code.
        /// </summary>
        /// <param name="deviceId">The device whose connection should be dropped.</param>
        /// <param name="reasonCode">The reason code to send.</param>
        /// <param name="reasonString">
        /// The optional reason string to accompany the reason code. Defaults to
        /// <see cref="StubConnectionDropOptions.ReasonString"/>.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>True if the device was connected and its connection was dropped, false if it was not connected.</returns>
        /// <remarks>
        /// A gen2 device speaks MQTT 5 and so is told the reason code by the protocol. A gen1 device speaks MQTT 3.1.1,
        /// which has no server-to-client DISCONNECT packet, so a real classic hub could only close the socket -
        /// <see cref="InProcessMqttBroker"/> is more generous and passes the code along anyway. Either way the drop is
        /// reported here and through <see cref="DeviceConnectionDropped"/>.
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
        /// Drop a device's connection with a reason code drawn at random from
        /// <see cref="StubConnectionDropOptions.ReasonCodes"/>.
        /// </summary>
        /// <param name="deviceId">The device whose connection should be dropped.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The reason code that was sent, or null if the device was not connected.</returns>
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
        /// Drop one randomly chosen connected device with one randomly chosen reason code.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>What was dropped, or null if no device this stub serves was connected.</returns>
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

        private void HandleConnectionDropped(StubDeviceConnectionDroppedEventArgs args)
        {
            // A gen2 device has to redo the presence handshake before the stub may dispatch to it again, exactly as it would
            // after any other disconnect.
            if (_options.Generation == IotHubGeneration.Gen2
                && _devices.TryGetValue(args.DeviceId, out StubDeviceState? device))
            {
                device.IsDispatchReady = false;
            }

            DeviceConnectionDropped?.Invoke(this, args);
        }
    }
}
