using MQTTnet.Protocol;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The MQTT 5 DISCONNECT reason codes a <see cref="StubIotHubService"/> can drop a device connection with.
    /// </summary>
    public static class StubDisconnectReasonCodes
    {
        /// <summary>
        /// Every reason code defined by <see cref="MqttDisconnectReasonCode"/>, including
        /// <see cref="MqttDisconnectReasonCode.DisconnectWithWillMessage"/>, which MQTT 5 only allows a client to send.
        /// </summary>
        /// <remarks>
        /// Use this to check that a device survives a protocol-illegal reason code as well as the legal ones.
        /// </remarks>
        public static IReadOnlyList<MqttDisconnectReasonCode> All { get; } = Enum.GetValues<MqttDisconnectReasonCode>();

        /// <summary>
        /// Every reason code a server is allowed to send in a DISCONNECT packet, per MQTT 5 section 3.14.2.1. That is every
        /// reason code except <see cref="MqttDisconnectReasonCode.DisconnectWithWillMessage"/>, which is client-only.
        /// </summary>
        public static IReadOnlyList<MqttDisconnectReasonCode> ServerInitiated { get; } =
            [.. Enum.GetValues<MqttDisconnectReasonCode>().Where(reasonCode => reasonCode != MqttDisconnectReasonCode.DisconnectWithWillMessage)];

        /// <summary>
        /// Every server-sendable reason code that reports a failure, which is <see cref="ServerInitiated"/> without
        /// <see cref="MqttDisconnectReasonCode.NormalDisconnection"/>.
        /// </summary>
        public static IReadOnlyList<MqttDisconnectReasonCode> ServerInitiatedErrors { get; } =
            [.. ServerInitiated.Where(reasonCode => reasonCode != MqttDisconnectReasonCode.NormalDisconnection)];
    }

    /// <summary>
    /// How a <see cref="StubIotHubService"/> randomly drops the connections of the devices it serves.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Dropping a connection requires an <see cref="StubIotHubServiceOptions.ConnectionDropper"/>, since the stub is an MQTT
    /// client rather than the broker. With one wired up and <see cref="Enabled"/> set, the stub runs a background loop from
    /// <see cref="StubIotHubService.StartAsync"/> until <see cref="StubIotHubService.StopAsync"/> that repeatedly waits a
    /// random interval in <c>[<see cref="MinInterval"/>, <see cref="MaxInterval"/>]</c>, rolls <see cref="Probability"/>,
    /// and on a hit drops one randomly chosen connected device with one randomly chosen code from
    /// <see cref="ReasonCodes"/>.
    /// </para>
    /// <para>
    /// Set <see cref="RandomSeed"/> to make a failing randomized run reproducible.
    /// </para>
    /// </remarks>
    /// <example>
    /// <code>
    /// var hub = new StubIotHubService(new StubIotHubServiceOptions
    /// {
    ///     ConnectionDropper = broker,
    ///     RandomConnectionDrops = new StubConnectionDropOptions
    ///     {
    ///         Enabled = true,
    ///         MinInterval = TimeSpan.FromMilliseconds(500),
    ///         MaxInterval = TimeSpan.FromSeconds(2),
    ///         RandomSeed = 1234,
    ///     },
    /// });
    /// </code>
    /// </example>
    public sealed class StubConnectionDropOptions
    {
        private IReadOnlyList<MqttDisconnectReasonCode> _reasonCodes = StubDisconnectReasonCodes.ServerInitiated;

        /// <summary>
        /// Whether the stub randomly drops device connections while it is started.
        /// </summary>
        /// <remarks>
        /// This only governs the background loop. <see cref="StubIotHubService.DropDeviceConnectionAsync"/> and
        /// <see cref="StubIotHubService.DropRandomDeviceConnectionAsync"/> can be called explicitly regardless of this value.
        /// </remarks>
        public bool Enabled { get; set; }

        /// <summary>
        /// The shortest wait between two drop attempts.
        /// </summary>
        public TimeSpan MinInterval { get; set; } = TimeSpan.FromSeconds(1);

        /// <summary>
        /// The longest wait between two drop attempts.
        /// </summary>
        public TimeSpan MaxInterval { get; set; } = TimeSpan.FromSeconds(5);

        /// <summary>
        /// The probability, in <c>[0, 1]</c>, that any one attempt actually drops a connection. A value below 1 makes the
        /// drops arrive in clusters and gaps rather than on a metronome.
        /// </summary>
        public double Probability { get; set; } = 1.0;

        /// <summary>
        /// The reason codes to choose from, uniformly at random. Defaults to
        /// <see cref="StubDisconnectReasonCodes.ServerInitiated"/>; assign <see cref="StubDisconnectReasonCodes.All"/> to
        /// include the client-only <see cref="MqttDisconnectReasonCode.DisconnectWithWillMessage"/>, or a list of one to pin
        /// the code a test sees.
        /// </summary>
        public IReadOnlyList<MqttDisconnectReasonCode> ReasonCodes
        {
            get => _reasonCodes;
            set
            {
                ArgumentNullException.ThrowIfNull(value);

                if (value.Count == 0)
                {
                    throw new ArgumentException("At least one MQTT disconnect reason code is required.", nameof(value));
                }

                _reasonCodes = value;
            }
        }

        /// <summary>
        /// Restrict the drops to these device ids. When null, every device the stub serves and knows about is a candidate.
        /// </summary>
        public IReadOnlyList<string>? DeviceIds { get; set; }

        /// <summary>
        /// The seed for the stub's random number generator, so that a randomized run can be replayed. When null the
        /// generator is seeded unpredictably.
        /// </summary>
        public int? RandomSeed { get; set; }

        /// <summary>
        /// Stop the background loop after this many drops. When null the loop runs until the stub is stopped.
        /// </summary>
        public int? MaxDrops { get; set; }

        /// <summary>
        /// The reason string to send alongside the reason code. When null the stub sends none.
        /// </summary>
        public string? ReasonString { get; set; }

        internal void Validate()
        {
            if (MinInterval < TimeSpan.Zero)
            {
                throw new InvalidOperationException($"{nameof(MinInterval)} must not be negative.");
            }

            if (MaxInterval < MinInterval)
            {
                throw new InvalidOperationException($"{nameof(MaxInterval)} must not be shorter than {nameof(MinInterval)}.");
            }

            if (Probability is < 0 or > 1)
            {
                throw new InvalidOperationException($"{nameof(Probability)} must be between 0 and 1 inclusive.");
            }

            if (MaxDrops is <= 0)
            {
                throw new InvalidOperationException($"{nameof(MaxDrops)} must be greater than zero when it is set.");
            }

            if (DeviceIds is { Count: 0 })
            {
                throw new InvalidOperationException($"{nameof(DeviceIds)} must contain at least one device id when it is set.");
            }
        }
    }
}
