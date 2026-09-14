using Microsoft.Azure.Devices.Client.Provisioning.Models;
using MQTTnet;
using MQTTnet.Protocol;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// An in-test stand-in for the Device Provisioning Service. It is an MQTT client that attaches to the same broker the devices
    /// under test connect to, subscribes to the registration topics, and answers them the way a real DPS would.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The stub always provisions successfully, and always assigns the device to the stub IoT hub. What it varies, through
    /// <see cref="StubDeviceProvisioningServiceOptions.Generation"/>, is which <em>kind</em> of hub it assigns the device to: the
    /// registration result's <see cref="DeviceRegistrationResult.ConnectionProfile"/> is what the SDK reads to choose
    /// between the classic MQTT 3.1.1 hub protocol and the gen2 MQTT 5 protocol. Provisioning itself is identical for both
    /// generations - DPS speaks MQTT 3.1.1 and JSON either way.
    /// </para>
    /// <para>
    /// The registration flow this implements, matching <c>ProvisioningConnection</c> in the SDK:
    /// </para>
    /// <list type="number">
    /// <item><description>The device publishes to <c>$dps/registrations/PUT/iotdps-register/?$rid={rid}</c>.</description></item>
    /// <item><description>The stub answers on <c>$dps/registrations/res/202/?$rid={rid}&amp;retry-after={n}</c> with status <c>assigning</c> and an operation id.</description></item>
    /// <item><description>The device polls <c>$dps/registrations/GET/iotdps-get-operationstatus/?$rid={rid}&amp;operationId={id}</c>.</description></item>
    /// <item><description>The stub answers on <c>$dps/registrations/res/200/?$rid={rid}</c> with status <c>assigned</c> and the registration state.</description></item>
    /// </list>
    /// <para>
    /// The stub is not a broker. Point it, the stub hub, and the devices under test at the same MQTT broker.
    /// </para>
    /// <para>
    /// Because it is not a broker, terminating the registering device's connection is something it has to borrow from one.
    /// Supply a <see cref="StubDeviceProvisioningServiceOptions.ConnectionDropper"/> - <see cref="InProcessMqttBroker"/> is
    /// one - and the stub can cut a registration short, either on demand through <see cref="DropDeviceConnectionAsync"/> or
    /// at random through <see cref="StubDeviceProvisioningServiceOptions.RandomConnectionDrops"/>.
    /// </para>
    /// </remarks>
    /// <example>
    /// <code>
    /// await using var hub = new StubIotHubService(hubOptions);
    /// await hub.StartAsync();
    ///
    /// await using var dps = StubDeviceProvisioningService.ForHub(hub, options => options.DeviceId = deviceId);
    /// await dps.StartAsync();
    ///
    /// // The device client now provisions and lands on the stub hub, using the generation the stub hub speaks.
    /// await connectionClient.ProvisionAndConnectAsync(new ProvisioningSettings(idScope), authentication);
    /// </code>
    /// </example>
    public sealed partial class StubDeviceProvisioningService : IAsyncDisposable
    {
        private const string RegisterTopicFilter = "$dps/registrations/PUT/iotdps-register/#";
        private const string OperationStatusTopicFilter = "$dps/registrations/GET/iotdps-get-operationstatus/#";
        private const string RegisterTopicPrefix = "$dps/registrations/PUT/iotdps-register/";
        private const string OperationStatusTopicPrefix = "$dps/registrations/GET/iotdps-get-operationstatus/";

        private readonly StubDeviceProvisioningServiceOptions _options;
        private readonly MQTTnet.IMqttClient _mqttClient;
        private readonly StubIotHubService? _hub;

        /// <summary>
        /// The registrations this stub has handled, keyed on the operation id it issued.
        /// </summary>
        private readonly ConcurrentDictionary<string, StubRegistrationOperation> _operations = new();

        private bool _isStarted;
        private bool _isDisposed;

        /// <summary>
        /// Construct a new stub DPS. No network activity happens until <see cref="StartAsync"/> is called.
        /// </summary>
        /// <param name="options">The stub's configuration. If null, the defaults on <see cref="StubDeviceProvisioningServiceOptions"/> are used.</param>
        public StubDeviceProvisioningService(StubDeviceProvisioningServiceOptions? options = null)
            : this(options, null)
        {
        }

        private StubDeviceProvisioningService(StubDeviceProvisioningServiceOptions? options, StubIotHubService? hub)
        {
            _options = options ?? new StubDeviceProvisioningServiceOptions();
            _hub = hub;
            _mqttClient = new MqttClientFactory().CreateMqttClient();
            _mqttClient.ApplicationMessageReceivedAsync += HandleReceivedMessageAsync;
        }

        /// <summary>
        /// Create a stub DPS that provisions devices onto the given stub hub.
        /// </summary>
        /// <remarks>
        /// The hub's generation, broker endpoint, and host name are copied into the stub's options before
        /// <paramref name="configure"/> runs, so the two stubs cannot drift out of agreement about which protocol the device
        /// should end up speaking. Devices this stub assigns are also registered with the hub, which means a gen1 hub can
        /// attribute the device-id-less classic topics without any further configuration.
        /// </remarks>
        /// <param name="hub">The stub hub to provision devices onto.</param>
        /// <param name="configure">An optional callback to further configure the stub, most usefully to set the device id.</param>
        public static StubDeviceProvisioningService ForHub(StubIotHubService hub, Action<StubDeviceProvisioningServiceOptions>? configure = null)
        {
            ArgumentNullException.ThrowIfNull(hub);

            var options = new StubDeviceProvisioningServiceOptions
            {
                Generation = hub.Generation,
                BrokerHostName = hub.BrokerHostName,
                BrokerPort = hub.BrokerPort,
                AssignedHubHostName = hub.HubHostName,

                // The hub and the DPS share a broker, so whatever closes sessions for one closes them for the other.
                ConnectionDropper = hub.ConnectionDropper,
            };

            configure?.Invoke(options);

            return new StubDeviceProvisioningService(options, hub);
        }

        /// <summary>
        /// Which generation of IoT hub this stub provisions devices to.
        /// </summary>
        public IotHubGeneration Generation => _options.Generation;

        /// <summary>
        /// Whether this stub is currently connected to the broker.
        /// </summary>
        public bool IsConnected => _mqttClient.IsConnected;

        /// <summary>
        /// Raised once a device's registration request has been answered with a terminal assignment.
        /// </summary>
        public event EventHandler<StubDeviceProvisionedEventArgs>? DeviceProvisioned;

        /// <summary>
        /// Connect this stub to the broker and subscribe to the registration topics.
        /// </summary>
        public async Task StartAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            if (_isStarted)
            {
                return;
            }

            // DPS only ever speaks MQTT 3.1.1, for both generations of hub.
            MqttClientOptionsBuilder optionsBuilder = new MqttClientOptionsBuilder()
                .WithProtocolVersion(MQTTnet.Formatter.MqttProtocolVersion.V311)
                .WithTcpServer(_options.BrokerHostName, _options.BrokerPort)
                .WithClientId(_options.ClientId)
                .WithCleanSession(true);

            if (_options.Username != null)
            {
                optionsBuilder.WithCredentials(_options.Username, _options.Password);
            }

            if (_options.UseTls || _options.ClientCertificate != null)
            {
                optionsBuilder.WithTlsOptions(tlsOptions =>
                {
                    tlsOptions.UseTls(true);

                    if (_options.ClientCertificate != null)
                    {
                        tlsOptions.WithClientCertificates([_options.ClientCertificate]);
                    }

                    if (_options.AllowUntrustedCertificates)
                    {
                        tlsOptions.WithAllowUntrustedCertificates(true);
                        tlsOptions.WithIgnoreCertificateChainErrors(true);
                        tlsOptions.WithCertificateValidationHandler(_ => true);
                    }
                });
            }

            await _mqttClient.ConnectAsync(optionsBuilder.Build(), cancellationToken);

            MqttClientSubscribeOptions subscribeOptions = new MqttClientSubscribeOptionsBuilder()
                .WithTopicFilter(filter => filter.WithTopic(RegisterTopicFilter).WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce))
                .WithTopicFilter(filter => filter.WithTopic(OperationStatusTopicFilter).WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce))
                .Build();

            MqttClientSubscribeResult subscribeResult = await _mqttClient.SubscribeAsync(subscribeOptions, cancellationToken);
            foreach (MqttClientSubscribeResultItem item in subscribeResult.Items)
            {
                if (item.ResultCode is not (MqttClientSubscribeResultCode.GrantedQoS0
                    or MqttClientSubscribeResultCode.GrantedQoS1
                    or MqttClientSubscribeResultCode.GrantedQoS2))
                {
                    throw new InvalidOperationException(
                        $"The stub DPS failed to subscribe to '{item.TopicFilter.Topic}'. Reason code: {item.ResultCode}.");
                }
            }

            _isStarted = true;
            Log($"Stub DPS is listening on {_options.BrokerHostName}:{_options.BrokerPort} and will assign devices to the "
                + $"{_options.Generation} hub '{_options.AssignedHubHostName}'.");

            ConnectionDrops.StartRandomDrops();
        }

        /// <summary>
        /// Disconnect this stub from the broker.
        /// </summary>
        public async Task StopAsync(CancellationToken cancellationToken = default)
        {
            if (!_isStarted)
            {
                return;
            }

            _isStarted = false;

            await ConnectionDrops.StopRandomDropsAsync();

            if (_mqttClient.IsConnected)
            {
                await _mqttClient.DisconnectAsync(new MqttClientDisconnectOptions(), cancellationToken);
            }
        }

        /// <summary>
        /// Releases the resources used by this stub, disconnecting from the broker first if necessary.
        /// </summary>
        public async ValueTask DisposeAsync()
        {
            if (_isDisposed)
            {
                return;
            }

            _isDisposed = true;

            try
            {
                await StopAsync(CancellationToken.None);
            }
            catch (Exception e)
            {
                Log($"Ignoring an exception while disconnecting the stub DPS: {e}");
            }

            _mqttClient.ApplicationMessageReceivedAsync -= HandleReceivedMessageAsync;
            _mqttClient.Dispose();
        }

        private async Task HandleReceivedMessageAsync(MqttApplicationMessageReceivedEventArgs args)
        {
            try
            {
                string topic = args.ApplicationMessage.Topic;

                if (topic.StartsWith(RegisterTopicPrefix, StringComparison.Ordinal))
                {
                    await HandleRegisterAsync(topic[RegisterTopicPrefix.Length..]);
                }
                else if (topic.StartsWith(OperationStatusTopicPrefix, StringComparison.Ordinal))
                {
                    await HandleOperationStatusAsync(topic[OperationStatusTopicPrefix.Length..]);
                }
                else
                {
                    Log($"Discarding a message on unrecognized DPS topic '{topic}'.");
                }
            }
            catch (Exception e)
            {
                // A real service never lets one malformed request take down its dispatch loop, and neither should the stub.
                Log($"The stub DPS failed to handle a message on '{args.ApplicationMessage.Topic}': {e}");
            }
        }

        private async Task HandleRegisterAsync(string propertyBag)
        {
            Dictionary<string, string> properties = ParseTopicPropertyBag(propertyBag);

            if (!properties.TryGetValue("$rid", out string? requestId))
            {
                Log("Discarding a DPS registration request because it carries no $rid.");
                return;
            }

            string deviceId = ResolveDeviceId();
            string operationId = Guid.NewGuid().ToString("N");

            _operations[operationId] = new StubRegistrationOperation(deviceId, _options.AssigningPollResponses);

            // A real DPS answers the registration request with 202 Accepted and an operation id to poll on. The device may
            // not be assigned yet at this point, so the status is always "assigning" here.
            var status = new RegistrationOperationStatus
            {
                OperationId = operationId,
                Status = ProvisioningRegistrationStatus.Assigning,
            };

            Log($"Accepted a DPS registration request (rid {requestId}) for device '{deviceId}' as operation {operationId}.");

            await PublishResponseAsync(202, requestId, includeRetryAfter: true, status);
        }

        private async Task HandleOperationStatusAsync(string propertyBag)
        {
            Dictionary<string, string> properties = ParseTopicPropertyBag(propertyBag);

            if (!properties.TryGetValue("$rid", out string? requestId))
            {
                Log("Discarding a DPS operation status request because it carries no $rid.");
                return;
            }

            if (!properties.TryGetValue("operationId", out string? operationId)
                || !_operations.TryGetValue(operationId, out StubRegistrationOperation? operation))
            {
                // A real DPS returns 404 for an operation it does not know about.
                Log($"Rejecting a DPS operation status request (rid {requestId}) for unknown operation '{operationId}'.");

                await PublishResponseAsync(
                    404,
                    requestId,
                    includeRetryAfter: false,
                    new RegistrationOperationStatus
                    {
                        OperationId = operationId,
                        Status = ProvisioningRegistrationStatus.Failed,
                        RegistrationState = new DeviceRegistrationResult
                        {
                            RegistrationId = _options.RegistrationId ?? _options.DeviceId ?? string.Empty,
                            Status = ProvisioningRegistrationStatus.Failed,
                            ErrorCode = 404,
                            ErrorMessage = "The stub DPS has no record of this operation.",
                        },
                    });

                return;
            }

            DateTimeOffset now = DateTimeOffset.UtcNow;

            if (operation.TryConsumeAssigningResponse())
            {
                // Still working on it. The device is expected to keep polling until a terminal state is reached.
                Log($"Answering DPS operation status request (rid {requestId}) for operation {operationId} with 'assigning'.");

                await PublishResponseAsync(
                    202,
                    requestId,
                    includeRetryAfter: true,
                    new RegistrationOperationStatus
                    {
                        OperationId = operationId,
                        Status = ProvisioningRegistrationStatus.Assigning,
                        RegistrationState = new DeviceRegistrationResult
                        {
                            RegistrationId = _options.RegistrationId ?? operation.DeviceId,
                            DeviceId = operation.DeviceId,
                            Status = ProvisioningRegistrationStatus.Assigning,
                            CreatedOnUtc = operation.CreatedOnUtc,
                            LastUpdatedOnUtc = now,
                        },
                    });

                return;
            }

            var registrationState = new DeviceRegistrationResult
            {
                RegistrationId = _options.RegistrationId ?? operation.DeviceId,
                DeviceId = operation.DeviceId,
                AssignedHub = _options.AssignedHubHostName,
                Status = ProvisioningRegistrationStatus.Assigned,
                Substatus = _options.Substatus,
                CreatedOnUtc = operation.CreatedOnUtc,
                LastUpdatedOnUtc = now,
                ETag = operation.ETag,

                // This is what the SDK reads to decide which generation of hub protocol to speak from here on.
                ConnectionProfile = _options.Generation == IotHubGeneration.Gen2
                    ? ConnectionProfile.MqttV5
                    : ConnectionProfile.Classic,
            };

            // Make the hub aware of the device before it connects. Besides being what a real assignment does, this lets a gen1
            // stub hub attribute the classic topics, which carry no device id, without any extra configuration.
            _hub?.GetDeviceState(operation.DeviceId);

            Log($"Assigned device '{operation.DeviceId}' to the {_options.Generation} hub "
                + $"'{_options.AssignedHubHostName}' (operation {operationId}, rid {requestId}).");

            await PublishResponseAsync(
                200,
                requestId,
                includeRetryAfter: false,
                new RegistrationOperationStatus
                {
                    OperationId = operationId,
                    Status = ProvisioningRegistrationStatus.Assigned,
                    RegistrationState = registrationState,
                });

            DeviceProvisioned?.Invoke(this, new StubDeviceProvisionedEventArgs
            {
                DeviceId = operation.DeviceId,
                RegistrationId = registrationState.RegistrationId,
                AssignedHubHostName = _options.AssignedHubHostName,
                Generation = _options.Generation,
            });
        }

        private Task PublishResponseAsync(int statusCode, string requestId, bool includeRetryAfter, RegistrationOperationStatus status)
        {
            string topic = $"$dps/registrations/res/{statusCode}/?$rid={Uri.EscapeDataString(requestId)}";

            if (includeRetryAfter)
            {
                int retryAfterSeconds = Math.Max(1, (int)Math.Round(_options.RetryAfter.TotalSeconds));
                topic += $"&retry-after={retryAfterSeconds.ToString(CultureInfo.InvariantCulture)}";
            }

            // Serialize with the SDK's own options so that the enum casing and property names are exactly what it deserializes.
            byte[] payload = JsonSerializer.SerializeToUtf8Bytes(status, JsonSerializationSettings.Options);

            return _mqttClient.PublishAsync(
                new MqttApplicationMessageBuilder()
                    .WithTopic(topic)
                    .WithPayload(payload)
                    .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                    .Build(),
                CancellationToken.None);
        }

        /// <summary>
        /// Works out which device id to assign.
        /// </summary>
        /// <remarks>
        /// A real DPS looks up the enrollment record for the registration id in the device's CONNECT username. The DPS MQTT
        /// topics carry no registration id at all, and a stub sharing a broker cannot see another client's CONNECT packet, so
        /// the device id has to come from configuration instead.
        /// </remarks>
        private string ResolveDeviceId()
        {
            string? deviceId = _options.DeviceId ?? _options.RegistrationId;

            return deviceId
                ?? throw new InvalidOperationException(
                    $"The stub DPS cannot tell which device is registering. The DPS MQTT topics carry no registration id, so set "
                    + $"{nameof(StubDeviceProvisioningServiceOptions)}.{nameof(StubDeviceProvisioningServiceOptions.DeviceId)}.");
        }

        private void Log(string message)
        {
            _options.Logger?.Invoke(message);
            Trace.TraceInformation(message);
        }

        /// <summary>
        /// Parses the query string that the DPS topics carry, for example the <c>?$rid=2&amp;operationId=abc</c> tail of
        /// <c>$dps/registrations/GET/iotdps-get-operationstatus/?$rid=2&amp;operationId=abc</c>.
        /// </summary>
        private static Dictionary<string, string> ParseTopicPropertyBag(string propertyBag)
        {
            var parsed = new Dictionary<string, string>(StringComparer.Ordinal);

            if (string.IsNullOrEmpty(propertyBag))
            {
                return parsed;
            }

            if (propertyBag[0] == '?')
            {
                propertyBag = propertyBag[1..];
            }

            foreach (string pair in propertyBag.Split('&', StringSplitOptions.RemoveEmptyEntries))
            {
                int separatorIndex = pair.IndexOf('=');
                if (separatorIndex < 0)
                {
                    parsed[Uri.UnescapeDataString(pair)] = string.Empty;
                    continue;
                }

                string key = Uri.UnescapeDataString(pair[..separatorIndex]);
                string value = Uri.UnescapeDataString(pair[(separatorIndex + 1)..]);
                parsed[key] = value;
            }

            return parsed;
        }

        /// <summary>
        /// The stub's record of one in-flight registration.
        /// </summary>
        private sealed class StubRegistrationOperation
        {
            private int _remainingAssigningResponses;

            public StubRegistrationOperation(string deviceId, int assigningResponses)
            {
                DeviceId = deviceId;
                _remainingAssigningResponses = assigningResponses;
            }

            public string DeviceId { get; }

            public DateTimeOffset CreatedOnUtc { get; } = DateTimeOffset.UtcNow;

            public string ETag { get; } = Guid.NewGuid().ToString("N");

            /// <summary>
            /// Returns true while the stub should still answer polls with <c>assigning</c>.
            /// </summary>
            public bool TryConsumeAssigningResponse()
            {
                while (true)
                {
                    int remaining = Volatile.Read(ref _remainingAssigningResponses);
                    if (remaining <= 0)
                    {
                        return false;
                    }

                    if (Interlocked.CompareExchange(ref _remainingAssigningResponses, remaining - 1, remaining) == remaining)
                    {
                        return true;
                    }
                }
            }
        }
    }

    /// <summary>
    /// Describes a device that the stub DPS has assigned to a hub.
    /// </summary>
    public class StubDeviceProvisionedEventArgs : EventArgs
    {
        /// <summary>
        /// The device id that was assigned.
        /// </summary>
        public required string DeviceId { get; init; }

        /// <summary>
        /// The registration id reported back to the device.
        /// </summary>
        public required string RegistrationId { get; init; }

        /// <summary>
        /// The hub the device was assigned to.
        /// </summary>
        public required string AssignedHubHostName { get; init; }

        /// <summary>
        /// Which generation of hub the device was told it was assigned to.
        /// </summary>
        public required IotHubGeneration Generation { get; init; }
    }
}
