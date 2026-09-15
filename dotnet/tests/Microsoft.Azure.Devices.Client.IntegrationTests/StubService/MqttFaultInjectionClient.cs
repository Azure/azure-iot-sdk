using MQTTnet;
using MQTTnet.Formatter;
using MQTTnet.Protocol;
using System.Buffers;
using System.Collections.Concurrent;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// An MQTT client that injects faults into an <see cref="InProcessMqttBroker"/> by publishing to
    /// <see cref="MqttFaultInjection.RequestTopic"/>, and the <see cref="IStubDeviceConnectionDropper"/> both stubs use to
    /// drop a device's connection.
    /// </summary>
    /// <remarks>
    /// <para>
    /// This class holds nothing but an MQTT connection to the broker, so it is a demonstration as much as a helper: every
    /// fault it triggers is triggered by bytes on the wire, and any other process - or any other language - can send the
    /// same bytes to the same topic and get the same result.
    /// </para>
    /// <para>
    /// The fault's structure travels in the PUBLISH payload rather than in MQTT 5 user properties, so
    /// <see cref="MqttProtocolVersion.V311"/> works exactly as well as <see cref="MqttProtocolVersion.V500"/>. Both are
    /// exercised by the tests, because a gen1 device and the gen1 stubs are on 3.1.1.
    /// </para>
    /// </remarks>
    /// <example>
    /// <code>
    /// await using var broker = await InProcessMqttBroker.StartAsync();
    /// await using var faults = await MqttFaultInjectionClient.ForBrokerAsync(broker);
    ///
    /// await faults.DisconnectClientAsync("device-1", MqttDisconnectReasonCode.ServerBusy);
    /// </code>
    /// </example>
    public sealed class MqttFaultInjectionClient : IStubDeviceConnectionDropper, IAsyncDisposable
    {
        private readonly IMqttClient _mqttClient;
        private readonly ConcurrentDictionary<string, TaskCompletionSource<MqttFaultInjectionResponse>> _pendingRequests = new();

        private bool _isDisposed;

        private MqttFaultInjectionClient(IMqttClient mqttClient, string clientId, MqttProtocolVersion protocolVersion)
        {
            _mqttClient = mqttClient;
            ClientId = clientId;
            ProtocolVersion = protocolVersion;

            _mqttClient.ApplicationMessageReceivedAsync += HandleResponseAsync;
        }

        /// <summary>
        /// The MQTT client id this client connected with.
        /// </summary>
        public string ClientId { get; }

        /// <summary>
        /// The MQTT version this client speaks to the broker. The fault injection protocol is the same either way.
        /// </summary>
        public MqttProtocolVersion ProtocolVersion { get; }

        /// <summary>
        /// How long to wait for the broker's response, on top of any delay the request itself asked for.
        /// </summary>
        public TimeSpan RequestTimeout { get; set; } = TimeSpan.FromSeconds(30);

        /// <summary>
        /// Connect a fault injection client to a broker.
        /// </summary>
        /// <param name="hostName">The broker's host name.</param>
        /// <param name="port">The broker's port.</param>
        /// <param name="protocolVersion">
        /// The MQTT version to connect with. Defaults to <see cref="MqttProtocolVersion.V311"/>, since the protocol
        /// deliberately needs nothing that MQTT 5 added.
        /// </param>
        /// <param name="clientId">The MQTT client id to connect with. One is generated when this is null.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        public static async Task<MqttFaultInjectionClient> StartAsync(
            string hostName,
            int port,
            MqttProtocolVersion protocolVersion = MqttProtocolVersion.V311,
            string? clientId = null,
            CancellationToken cancellationToken = default)
        {
            ArgumentException.ThrowIfNullOrEmpty(hostName);

            clientId ??= "fault-injection-client-" + Guid.NewGuid().ToString("N");

            IMqttClient mqttClient = new MqttClientFactory().CreateMqttClient();
            var client = new MqttFaultInjectionClient(mqttClient, clientId, protocolVersion);

            try
            {
                MqttClientOptions options = new MqttClientOptionsBuilder()
                    .WithProtocolVersion(protocolVersion)
                    .WithTcpServer(hostName, port)
                    .WithClientId(clientId)
                    .WithCleanSession(true)
                    .Build();

                await mqttClient.ConnectAsync(options, cancellationToken);

                // Subscribing to every response rather than to one per request keeps a request down to a single publish,
                // which is what an out-of-process caller would want to send.
                MqttClientSubscribeResult subscribeResult = await mqttClient.SubscribeAsync(
                    new MqttClientSubscribeOptionsBuilder()
                        .WithTopicFilter(filter => filter
                            .WithTopic(MqttFaultInjection.ResponseTopicFilter)
                            .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce))
                        .Build(),
                    cancellationToken);

                foreach (MqttClientSubscribeResultItem item in subscribeResult.Items)
                {
                    if (item.ResultCode is not (MqttClientSubscribeResultCode.GrantedQoS0
                        or MqttClientSubscribeResultCode.GrantedQoS1
                        or MqttClientSubscribeResultCode.GrantedQoS2))
                    {
                        throw new InvalidOperationException(
                            $"The fault injection client failed to subscribe to '{item.TopicFilter.Topic}'. Reason code: {item.ResultCode}.");
                    }
                }

                return client;
            }
            catch
            {
                await client.DisposeAsync();
                throw;
            }
        }

        /// <summary>
        /// Connect a fault injection client to an in-process broker.
        /// </summary>
        public static Task<MqttFaultInjectionClient> ForBrokerAsync(
            InProcessMqttBroker broker,
            MqttProtocolVersion protocolVersion = MqttProtocolVersion.V311,
            string? clientId = null,
            CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(broker);

            return StartAsync(broker.HostName, broker.Port, protocolVersion, clientId, cancellationToken);
        }

        /// <summary>
        /// Ask the broker for the client ids that currently hold a session.
        /// </summary>
        public async Task<IReadOnlyList<string>> GetConnectedClientIdsAsync(CancellationToken cancellationToken = default)
        {
            MqttFaultInjectionResponse response = await SendAsync(
                new MqttFaultInjectionRequest { Fault = MqttFaultInjection.Faults.ListClients },
                cancellationToken);

            return response.ClientIds ?? [];
        }

        /// <summary>
        /// Ask the broker to terminate a client's session with the given MQTT disconnect reason code.
        /// </summary>
        /// <param name="clientId">The client whose session should be terminated.</param>
        /// <param name="reasonCode">The MQTT 5 DISCONNECT reason code to send.</param>
        /// <param name="reasonString">The optional human readable reason string to accompany the reason code.</param>
        /// <param name="delay">How long the broker should wait before disconnecting the client.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>True if a session was found and terminated, false if no such client was connected.</returns>
        public async Task<bool> DisconnectClientAsync(
            string clientId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString = null,
            TimeSpan delay = default,
            CancellationToken cancellationToken = default)
        {
            ArgumentException.ThrowIfNullOrEmpty(clientId);

            MqttFaultInjectionResponse response = await SendAsync(
                new MqttFaultInjectionRequest
                {
                    Fault = MqttFaultInjection.Faults.Disconnect,
                    ClientId = clientId,
                    ReasonCode = (int)reasonCode,
                    ReasonString = reasonString,
                    DelayMilliseconds = (int)delay.TotalMilliseconds,
                },
                cancellationToken);

            return response.FaultApplied;
        }

        /// <summary>
        /// Publish a fault injection request and wait for the broker to report what it did.
        /// </summary>
        /// <remarks>
        /// The request id is filled in here if the caller did not supply one, because it is what the response is correlated
        /// on - and what keeps this usable over MQTT 3.1.1, which has neither correlation data nor a response topic
        /// property.
        /// </remarks>
        /// <exception cref="InvalidOperationException">The broker rejected the request.</exception>
        public async Task<MqttFaultInjectionResponse> SendAsync(
            MqttFaultInjectionRequest request,
            CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentNullException.ThrowIfNull(request);

            request.RequestId ??= Guid.NewGuid().ToString("N");

            TaskCompletionSource<MqttFaultInjectionResponse> pending = new(TaskCreationOptions.RunContinuationsAsynchronously);
            if (!_pendingRequests.TryAdd(request.RequestId, pending))
            {
                throw new InvalidOperationException($"A fault injection request with id '{request.RequestId}' is already in flight.");
            }

            try
            {
                await _mqttClient.PublishAsync(
                    new MqttApplicationMessageBuilder()
                        .WithTopic(MqttFaultInjection.RequestTopic)
                        .WithPayload(MqttFaultInjection.Serialize(request))
                        .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                        .Build(),
                    cancellationToken);

                TimeSpan timeout = RequestTimeout + TimeSpan.FromMilliseconds(Math.Max(0, request.DelayMilliseconds));
                MqttFaultInjectionResponse response = await pending.Task.WaitAsync(timeout, cancellationToken);

                return !response.Succeeded
                    ? throw new InvalidOperationException(
                        $"The broker rejected the '{request.Fault}' fault injection request: {response.Error}")
                    : response;
            }
            finally
            {
                _pendingRequests.TryRemove(request.RequestId, out _);
            }
        }

        /// <summary>
        /// The device-oriented view of <see cref="GetConnectedClientIdsAsync(CancellationToken)"/>. The SDK connects a
        /// device with its device id as the MQTT client id, so the two are the same string.
        /// </summary>
        Task<IReadOnlyList<string>> IStubDeviceConnectionDropper.GetConnectedDeviceIdsAsync(CancellationToken cancellationToken)
        {
            return GetConnectedClientIdsAsync(cancellationToken);
        }

        /// <summary>
        /// The device-oriented view of
        /// <see cref="DisconnectClientAsync(string, MqttDisconnectReasonCode, string, TimeSpan, CancellationToken)"/>.
        /// </summary>
        Task<bool> IStubDeviceConnectionDropper.DropDeviceConnectionAsync(
            string deviceId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString,
            CancellationToken cancellationToken)
        {
            return DisconnectClientAsync(deviceId, reasonCode, reasonString, delay: default, cancellationToken);
        }

        public async ValueTask DisposeAsync()
        {
            if (_isDisposed)
            {
                return;
            }

            _isDisposed = true;
            _mqttClient.ApplicationMessageReceivedAsync -= HandleResponseAsync;

            foreach (TaskCompletionSource<MqttFaultInjectionResponse> pending in _pendingRequests.Values)
            {
                pending.TrySetCanceled();
            }

            _pendingRequests.Clear();

            try
            {
                if (_mqttClient.IsConnected)
                {
                    await _mqttClient.DisconnectAsync(new MqttClientDisconnectOptions(), CancellationToken.None);
                }
            }
            catch (Exception)
            {
                // Teardown of a test helper should never mask the test's own failure.
            }

            _mqttClient.Dispose();
        }

        private Task HandleResponseAsync(MqttApplicationMessageReceivedEventArgs args)
        {
            if (!args.ApplicationMessage.Topic.StartsWith(MqttFaultInjection.ResponseTopicPrefix, StringComparison.Ordinal))
            {
                return Task.CompletedTask;
            }

            MqttFaultInjectionResponse? response = MqttFaultInjection.DeserializeResponse(args.ApplicationMessage.Payload.ToArray());
            if (response?.RequestId == null)
            {
                return Task.CompletedTask;
            }

            // Responses to other clients' requests arrive here too, since the subscription is for all of them.
            if (_pendingRequests.TryGetValue(response.RequestId, out TaskCompletionSource<MqttFaultInjectionResponse>? pending))
            {
                pending.TrySetResult(response);
            }

            return Task.CompletedTask;
        }
    }
}
