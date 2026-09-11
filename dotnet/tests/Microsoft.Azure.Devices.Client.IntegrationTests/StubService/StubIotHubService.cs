using MQTTnet;
using MQTTnet.Protocol;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// An in-test stand-in for the IoT hub service. It is an MQTT client that attaches to the same broker the devices under test
    /// connect to, subscribes to the service-bound topics, and answers device traffic the way a real IoT hub would.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The stub is configurable, through <see cref="StubIotHubServiceOptions.Generation"/>, to behave like either generation of hub:
    /// </para>
    /// <list type="bullet">
    /// <item>
    /// <description>
    /// <see cref="IotHubGeneration.Gen1"/> - the classic hub. The stub connects with MQTT 3.1.1, serves the <c>$iothub/twin/GET</c>,
    /// <c>$iothub/twin/PATCH/properties/reported</c> and <c>$iothub/methods</c> topics, correlates via the <c>$rid</c> topic query
    /// parameter, and exchanges JSON.
    /// </description>
    /// </item>
    /// <item>
    /// <description>
    /// <see cref="IotHubGeneration.Gen2"/> - the hub built on the Azure Event Grid MQTT broker. The stub connects with MQTT 5, serves
    /// the <c>ih/{deviceId}/srv/{feature}</c> topics, correlates via MQTT 5 correlation data, dispatches on the <c>type</c> user
    /// property, and exchanges protobuf. This half is modeled on the presence, twin and direct method design documents under the
    /// repository's "iot hub service design docs" folder.
    /// </description>
    /// </item>
    /// </list>
    /// <para>
    /// The stub is not a broker. Point it and the devices under test at the same MQTT broker.
    /// </para>
    /// </remarks>
    /// <example>
    /// <code>
    /// await using var hub = new StubIotHubService(new StubIotHubServiceOptions
    /// {
    ///     Generation = IotHubGeneration.Gen2,
    ///     BrokerHostName = "localhost",
    ///     BrokerPort = 1883,
    /// });
    ///
    /// hub.GetDeviceState(deviceId).ReplaceDesiredProperties(new JsonObject { ["fanSpeed"] = 42 });
    /// await hub.StartAsync();
    ///
    /// // ... connect the device under test, then drive service-initiated flows ...
    /// await hub.UpdateDesiredPropertiesAsync(deviceId, new JsonObject { ["fanSpeed"] = 43 });
    /// StubDirectMethodResult result = await hub.InvokeDirectMethodAsync(deviceId, "reboot", payload);
    /// </code>
    /// </example>
    public sealed partial class StubIotHubService : IAsyncDisposable
    {
        private const string ProtobufContentType = "application/protobuf";

        private readonly StubIotHubServiceOptions _options;
        private readonly MQTTnet.IMqttClient _mqttClient;
        private readonly ConcurrentDictionary<string, StubDeviceState> _devices = new();
        private readonly ConcurrentDictionary<string, TaskCompletionSource<StubDeviceBirthEventArgs>> _birthWaiters = new();

        private bool _isStarted;
        private bool _isDisposed;

        /// <summary>
        /// Construct a new stub hub. No network activity happens until <see cref="StartAsync"/> is called, so device state such as
        /// an initial twin can be seeded first.
        /// </summary>
        /// <param name="options">The stub's configuration. If null, the defaults on <see cref="StubIotHubServiceOptions"/> are used.</param>
        public StubIotHubService(StubIotHubServiceOptions? options = null)
        {
            _options = options ?? new StubIotHubServiceOptions();
            _mqttClient = new MqttClientFactory().CreateMqttClient();
            _mqttClient.ApplicationMessageReceivedAsync += HandleReceivedMessageAsync;
        }

        /// <summary>
        /// Which generation of hub this stub is behaving like.
        /// </summary>
        public IotHubGeneration Generation => _options.Generation;

        /// <summary>
        /// The host name this stub hub presents itself as. See <see cref="StubIotHubServiceOptions.HubHostName"/>.
        /// </summary>
        public string HubHostName => _options.HubHostName;

        /// <summary>
        /// The host name of the broker this stub is attached to.
        /// </summary>
        public string BrokerHostName => _options.BrokerHostName;

        /// <summary>
        /// The port of the broker this stub is attached to.
        /// </summary>
        public int BrokerPort => _options.BrokerPort;

        /// <summary>
        /// Whether this stub is currently connected to the broker.
        /// </summary>
        public bool IsConnected => _mqttClient.IsConnected;

        /// <summary>
        /// Raised when a device sends telemetry.
        /// </summary>
        public event EventHandler<StubTelemetryReceivedEventArgs>? TelemetryReceived;

        /// <summary>
        /// Raised after a device's reported-properties patch has been handled.
        /// </summary>
        public event EventHandler<StubReportedPropertiesReceivedEventArgs>? ReportedPropertiesReceived;

        /// <summary>
        /// Raised after a device's twin GET has been answered.
        /// </summary>
        public event EventHandler<StubTwinGetReceivedEventArgs>? TwinGetReceived;

        /// <summary>
        /// Raised when a device's birth message is admitted. Gen2 only; the classic hub has no presence handshake.
        /// </summary>
        public event EventHandler<StubDeviceBirthEventArgs>? DeviceBirthReceived;

        /// <summary>
        /// Connect this stub to the broker and subscribe to the service-bound topics for the configured generation.
        /// </summary>
        public async Task StartAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            if (_isStarted)
            {
                return;
            }

            MqttClientOptionsBuilder optionsBuilder = new MqttClientOptionsBuilder()
                .WithProtocolVersion(_options.Generation == IotHubGeneration.Gen2
                    ? MQTTnet.Formatter.MqttProtocolVersion.V500
                    : MQTTnet.Formatter.MqttProtocolVersion.V311)
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

            MqttClientSubscribeOptionsBuilder subscribeBuilder = new();
            foreach (string topicFilter in GetServiceBoundTopicFilters())
            {
                subscribeBuilder.WithTopicFilter(filter => filter
                    .WithTopic(topicFilter)
                    .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce));
            }

            MqttClientSubscribeResult subscribeResult = await _mqttClient.SubscribeAsync(subscribeBuilder.Build(), cancellationToken);
            foreach (MqttClientSubscribeResultItem item in subscribeResult.Items)
            {
                if (item.ResultCode is not (MqttClientSubscribeResultCode.GrantedQoS0
                    or MqttClientSubscribeResultCode.GrantedQoS1
                    or MqttClientSubscribeResultCode.GrantedQoS2))
                {
                    throw new InvalidOperationException(
                        $"The stub IoT hub service failed to subscribe to '{item.TopicFilter.Topic}'. Reason code: {item.ResultCode}.");
                }
            }

            _isStarted = true;
            Log($"Stub {_options.Generation} IoT hub service is listening on {_options.BrokerHostName}:{_options.BrokerPort}.");
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

            if (_mqttClient.IsConnected)
            {
                await _mqttClient.DisconnectAsync(new MqttClientDisconnectOptions(), cancellationToken);
            }
        }

        /// <summary>
        /// Get the stub's authoritative state for a device, creating a default (empty twin at version 1) entry if this is the
        /// first time the device has been referenced.
        /// </summary>
        public StubDeviceState GetDeviceState(string deviceId)
        {
            ArgumentException.ThrowIfNullOrEmpty(deviceId);

            return _devices.GetOrAdd(deviceId, id => new StubDeviceState(id)
            {
                // The classic hub has no presence handshake, so a gen1 device is dispatchable as soon as it exists.
                IsDispatchReady = _options.Generation == IotHubGeneration.Gen1,
            });
        }

        /// <summary>
        /// Wait until the given device completes the presence handshake with this stub. Gen2 only.
        /// </summary>
        /// <remarks>
        /// This completes on the first admitted birth for the device and stays completed thereafter, so it is a one-shot
        /// "has this device come online" gate rather than a per-reconnect signal.
        /// </remarks>
        public async Task<StubDeviceBirthEventArgs> WaitForDeviceBirthAsync(string deviceId, CancellationToken cancellationToken = default)
        {
            ArgumentException.ThrowIfNullOrEmpty(deviceId);

            if (_options.Generation != IotHubGeneration.Gen2)
            {
                throw new NotSupportedException("Only a gen2 IoT hub performs a presence handshake with its devices.");
            }

            TaskCompletionSource<StubDeviceBirthEventArgs> waiter = GetBirthWaiter(deviceId);
            return await waiter.Task.WaitAsync(cancellationToken);
        }

        /// <summary>
        /// Apply a JSON merge patch to a device's desired properties and dispatch the patch to the device.
        /// </summary>
        /// <param name="deviceId">The device whose desired properties should change.</param>
        /// <param name="patch">The patch to apply. A null property value removes that property.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The new authoritative desired properties version.</returns>
        public async Task<ulong> UpdateDesiredPropertiesAsync(string deviceId, JsonObject patch, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentException.ThrowIfNullOrEmpty(deviceId);
            ArgumentNullException.ThrowIfNull(patch);
            EnsureStarted();

            StubDeviceState device = GetDeviceState(deviceId);
            ulong newVersion = device.ApplyDesiredPatch(patch);

            if (_options.Generation == IotHubGeneration.Gen2)
            {
                await PublishGen2DesiredPatchAsync(device, patch, newVersion, cancellationToken);
            }
            else
            {
                await PublishGen1DesiredPatchAsync(deviceId, patch, newVersion, cancellationToken);
            }

            return newVersion;
        }

        /// <summary>
        /// Invoke a direct method on a device and wait for its result.
        /// </summary>
        /// <param name="deviceId">The device to invoke the method on.</param>
        /// <param name="methodName">The method name.</param>
        /// <param name="payload">The opaque method parameters.</param>
        /// <param name="connectTimeout">
        /// How long the device has to acknowledge the delivery probe. Gen2 only; defaults to
        /// <see cref="StubIotHubServiceOptions.DefaultDirectMethodConnectTimeout"/>.
        /// </param>
        /// <param name="responseTimeout">
        /// How long the device has to return a result. Defaults to <see cref="StubIotHubServiceOptions.DefaultDirectMethodResponseTimeout"/>.
        /// </param>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task<StubDirectMethodResult> InvokeDirectMethodAsync(
            string deviceId,
            string methodName,
            byte[]? payload = null,
            TimeSpan? connectTimeout = null,
            TimeSpan? responseTimeout = null,
            CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentException.ThrowIfNullOrEmpty(deviceId);
            ArgumentException.ThrowIfNullOrEmpty(methodName);
            EnsureStarted();

            TimeSpan effectiveConnectTimeout = connectTimeout ?? _options.DefaultDirectMethodConnectTimeout;
            TimeSpan effectiveResponseTimeout = responseTimeout ?? _options.DefaultDirectMethodResponseTimeout;

            return _options.Generation == IotHubGeneration.Gen2
                ? await InvokeGen2DirectMethodAsync(deviceId, methodName, payload, effectiveConnectTimeout, effectiveResponseTimeout, cancellationToken)
                : await InvokeGen1DirectMethodAsync(deviceId, methodName, payload, effectiveResponseTimeout, cancellationToken);
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
                Log($"Ignoring an exception while disconnecting the stub IoT hub service: {e}");
            }

            _mqttClient.ApplicationMessageReceivedAsync -= HandleReceivedMessageAsync;
            _mqttClient.Dispose();
        }

        private string[] GetServiceBoundTopicFilters()
        {
            string deviceSegment = _options.DeviceIdFilter ?? "+";

            if (_options.Generation == IotHubGeneration.Gen2)
            {
                // Everything a gen2 device sends to the service lands under ih/{deviceId}/srv/...
                return [$"ih/{deviceSegment}/srv/#"];
            }

            return
            [
                $"devices/{deviceSegment}/messages/events/#",
                "$iothub/twin/GET/#",
                "$iothub/twin/PATCH/properties/reported/#",
                "$iothub/methods/res/#",
            ];
        }

        private async Task HandleReceivedMessageAsync(MqttApplicationMessageReceivedEventArgs args)
        {
            try
            {
                if (_options.Generation == IotHubGeneration.Gen2)
                {
                    await HandleGen2MessageAsync(args);
                }
                else
                {
                    await HandleGen1MessageAsync(args);
                }
            }
            catch (Exception e)
            {
                // A real hub never lets one malformed device message take down its dispatch loop, and neither should the stub.
                Log($"The stub IoT hub service failed to handle a message on '{args.ApplicationMessage.Topic}': {e}");
            }
        }

        private Task PublishAsync(MqttApplicationMessage message, CancellationToken cancellationToken)
        {
            return _mqttClient.PublishAsync(message, cancellationToken);
        }

        private void EnsureStarted()
        {
            if (!_isStarted)
            {
                throw new InvalidOperationException($"{nameof(StartAsync)} must be called before the stub IoT hub service can send anything.");
            }
        }

        private TaskCompletionSource<StubDeviceBirthEventArgs> GetBirthWaiter(string deviceId)
        {
            return _birthWaiters.GetOrAdd(deviceId, _ => new TaskCompletionSource<StubDeviceBirthEventArgs>(TaskCreationOptions.RunContinuationsAsynchronously));
        }

        private bool IsServedDevice(string deviceId)
        {
            return _options.DeviceIdFilter == null || _options.DeviceIdFilter.Equals(deviceId, StringComparison.Ordinal);
        }

        private void Log(string message)
        {
            _options.Logger?.Invoke(message);
            Trace.TraceInformation(message);
        }

        /// <summary>
        /// Parses the query string that classic IoT hub topics carry, for example the <c>?$rid=abc&amp;$version=2</c> tail of
        /// <c>$iothub/twin/res/204/?$rid=abc&amp;$version=2</c>.
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
    }
}
