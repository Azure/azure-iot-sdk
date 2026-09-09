using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client
{
    //TODO Does my setup already allow for user to publish via feature clients even when this client is connected to DPS during identity terminal exception handling? It does, right?

    public abstract class AbstractConnectionClient
    {
        private const string ProvisioningUsernameFormat = "{0}/registrations/{1}/api-version={2}&ClientVersion={3}";
        private const string ProvisioningApiVersion = "2019-03-31";
        private const string ProvisioningSubscribeFilter = "$dps/registrations/res/#";
        private const string ProvisioningRegisterTopic = "$dps/registrations/PUT/iotdps-register/?$rid={0}";
        private const string ProvisioningGetOperationsTopic = "$dps/registrations/GET/iotdps-get-operationstatus/?$rid={0}&operationId={1}";
        private const string RetryAfterHeader = "Retry-After";

        private static readonly TimeSpan s_defaultOperationPollingInterval = TimeSpan.FromSeconds(2);

        // The abstract methods cover all the differences between a gen2 client and a unified client.
        public abstract MqttConnect MqttConnectOverride(MqttConnect connect);

        // In gen2 case, SUB to devicebound, send birth message, wait for birth ack. In gen1 case, send all DM/Twin/Telem SUBs.
        // In both cases, this method should trigger the "OnDevicePresenceFlowCompleted" callback
        public abstract Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args);

        internal event Func<DevicePresenceFlowCompletedArgs, Task>? DevicePresenceFlowCompletedAsync;

        // Workaround so that inheriting classes can invoke the "DevicePresenceFlowCompletedAsync" event.
        protected Task RaiseDevicePresenceFlowCompletedAsync(DevicePresenceFlowCompletedArgs args)
        {
            if (DevicePresenceFlowCompletedAsync != null)
            {
                DevicePresenceFlowCompletedAsync.Invoke(args);
            }

            return Task.CompletedTask;
        }

        /// <summary>
        /// Raised once the provisioning flow that runs upon connecting to Device Provisioning Service has either
        /// produced a registration result or failed. This is the provisioning counterpart of
        /// <see cref="DevicePresenceFlowCompletedAsync"/>.
        /// </summary>
        private event Func<ProvisioningFlowCompletedArgs, Task>? ProvisioningFlowCompletedAsync;

        internal bool _isDisposed = false;
        private bool _isUserSuppliedMqttClient = false;

        internal MqttConnectionManager ManagedMqttConnection;

        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        public event Func<ConnectionFaultedEventArgs, Task>? ConnectionFaultedAsync;

        internal ConnectionContext? CurrentConnectionContext { get; set; }

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        /// <summary>
        /// The endpoint that this client is currently connecting to, or connected to.
        /// </summary>
        /// <remarks>
        /// This decides which flow runs when a connection is established: connecting to Device Provisioning Service
        /// starts the provisioning flow, while connecting to an IoT hub starts the device presence flow. The connection
        /// layer owns reconnection for both endpoints, so this also decides which flow a reconnection restarts.
        /// </remarks>
        public ConnectionEndpoint CurrentEndpoint { get; private set; } = ConnectionEndpoint.None;

        // The registration request to send on every connection to Device Provisioning Service. Only set while provisioning.
        private RegistrationRequestPayload? _provisioningRequestPayload;

        // The DPS responses that the in-progress provisioning flow is waiting on. Both are reset for each new connection
        // to DPS since a registration only lives as long as the connection it was started on.
        private TaskCompletionSource<RegistrationOperationStatus>? _startProvisioningRequestStatusSource;
        private TaskCompletionSource<RegistrationOperationStatus>? _checkRegistrationOperationStatusSource;
        private int _provisioningRequestId;

        /// <summary>
        /// Cancels everything the in-progress provisioning flow is waiting on. Because DPS cannot persist sessions, a
        /// flow is only valid for the connection it was started on, so this is cancelled whenever that connection ends.
        /// </summary>
        private CancellationTokenSource? _currentProvisioningFlowCancellation;







        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        public AbstractConnectionClient(ConnectionClientOptions? options = null)
        {
            options ??= new ConnectionClientOptions();

            // This is the basic MQTT client that has no reconnection/retry logic
            var unmanagedMqttClient = options.MqttClient ?? new MqttNetClient(enableMqttLogs: options.EnableMqttLogging);

            // This is the wrapper that manages reconnection
            ManagedMqttConnection = new(unmanagedMqttClient, options.ConnectionAttemptTimeout, options.ConnectionRetryPolicy);
            ManagedMqttConnection.PublishReceivedAsync += DelegatePublishAsync; // relay all publishes from the underlying MQTT client to users of this connection client

            // These handlers stay attached for the lifetime of this client and dispatch on the endpoint that the current
            // connection targets, so that a connection to DPS runs the provisioning flow and a connection to IoT hub runs
            // the device presence flow.
            ManagedMqttConnection.ConnectingAsync += PatchConnectPacketAsync;
            ManagedMqttConnection.ConnectedAsync += HandleConnectedAsync;
            ManagedMqttConnection.DisconnectedAsync += HandleDisconnectedAsync;
            ManagedMqttConnection.ConnectionFaultedAsync += HandleConnectionFaultedAsync;
            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedProvisioningPublishAsync;
        }

        private async Task DelegatePublishAsync(MqttPublishReceivedEventArgs args)
        {
            if (PublishReceivedAsync != null)
            {
                await PublishReceivedAsync.Invoke(args);
            }
        }

        /// <summary>
        /// Provision this device with the provided credentials using Device Provisioning Service, then connect this device to the IoT hub it was provisioned to.
        /// </summary>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields</param>
        /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
        /// <param name="twinOptions">The optional flags to control twin updates to this device from IoT hub.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The received twin push upon connecting to IoT hub if any part of the twin was configured to be pushed in <see cref="TwinPushOptions"/>.</returns>
        public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId!,
                IotHubHostName = provisioningResult.AssignedHub!,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                AuthenticationProvider = authentication,
                IsGen2Hub = provisioningResult.IsAzureEventGridHub,
            };

            await ConnectAsync(CurrentConnectionContext, null, cancellationToken);

            return CurrentConnectionContext;
        }

        /// <summary>
        /// Patch the CONNECT packet that is about to be sent, including on every reconnect attempt.
        /// </summary>
        private Task<MqttConnect> PatchConnectPacketAsync(MqttConnect connectPacketToEdit)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                // The CONNECT sent to DPS is fully formed before the first connect attempt and nothing about it changes
                // between attempts or between the generation of Hub that the inheriting class uses, so there is nothing to inject here.
                return Task.FromResult(connectPacketToEdit);
            }

            // Gen 1 hub clients and gen 2 hub clients can insert their specific username/password/protocol version/etc before each connect attempt
            return Task.FromResult(MqttConnectOverride(connectPacketToEdit));
        }

        /// <summary>
        /// Run the flow that the endpoint of the newly established connection expects.
        /// </summary>
        private async Task HandleConnectedAsync(MqttClientConnectedEventArgs args)
        {
            switch (CurrentEndpoint)
            {
                case ConnectionEndpoint.DeviceProvisioningService:
                    await HandleConnectedToDpsAsync(args);
                    break;
                case ConnectionEndpoint.IotHub:
                    await HandleConnectedToHubAsync(args);
                    break;
                default:
                    Trace.TraceWarning("Connection was established while this client was not targeting any endpoint. Ignoring it.");
                    break;
            }
        }

        private Task HandleDisconnectedAsync(MqttClientDisconnectedEventArgs args)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                // Because DPS cannot persist sessions, any connection loss should be treated as a session loss. Abandon
                // the in-progress flow so that it can be restarted from the beginning once the connection is re-established.
                CancelCurrentProvisioningFlow();
            }

            return Task.CompletedTask;
        }

        private async Task HandleConnectionFaultedAsync(MqttConnectionFaultedEventArgs args)
        {
            if (CurrentEndpoint == ConnectionEndpoint.DeviceProvisioningService)
            {
                // The connection layer has stopped maintaining the connection to DPS, so no further connection will
                // arrive to start the provisioning flow again.
                CancelCurrentProvisioningFlow();
                if (ProvisioningFlowCompletedAsync != null)
                {
                    await ProvisioningFlowCompletedAsync.Invoke(new ProvisioningFlowCompletedArgs(args.Exception));
                }
            }
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            await ManagedMqttConnection.DisconnectAsync(false, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection }, cancellationToken);
            CurrentConnectionContext = null;
            CurrentEndpoint = ConnectionEndpoint.None;
        }



        internal async Task ConnectAsync(ConnectionContext connectionContext, TwinPushOptions? twinPushOptions = default, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // From here on, every connection this client establishes targets IoT hub, so every connection (including the
            // ones the connection layer re-establishes on its own) runs the device presence flow.
            CurrentEndpoint = ConnectionEndpoint.IotHub;

            CurrentConnectionContext = connectionContext;

            string deviceId = CurrentConnectionContext.DeviceId;
            string hostname = CurrentConnectionContext.IotHubHostName;

            cancellationToken.ThrowIfCancellationRequested();

            Trace.TraceInformation("Attempting to establish connection and presence for device {0} with IoT Hub {1}", deviceId, hostname);

            TaskCompletionSource<DevicePresenceFlowCompletedArgs> devicePresenceFlowResult = new();
            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompleted = (args) =>
            {
                devicePresenceFlowResult.TrySetResult(args);
                return Task.CompletedTask;
            };

            // Setup callbacks BEFORE sending CONNECT so that CONNACK can be handled regardless of how quickly it arrives
            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;

            try
            {
                // These are the connect packet fields that are invariable between connect attempts to any kind of IoT Hub. There is an MQTT connect packet override
                // step triggered by the "ConnectingAsync" callback each time a connect is attempted. This allows for the gen 2 client to insert a new connect nonce
                // per connect attempt. It also allows the inheriting ConnectionClient to insert the appropriate username/password/websocket port for that hub type
                MqttConnect connectPacket = new MqttConnect()
                {
                    HostName = hostname,
                    TcpPort = 8883,
                    WebsocketPort = 443,
                    ClientCertificate = CurrentConnectionContext.AuthenticationProvider.ClientCertificate,
                    CleanSession = true, // TODO user configurable? Less applicable in gen 2 hub connection
                    ClientId = deviceId,
                };

                MqttConnectAck connack = await ManagedMqttConnection.ConnectAsync(connectPacket, cancellationToken);

                var devicePresenceFlowCompletedArgs = await devicePresenceFlowResult.Task.WaitAsync(cancellationToken);

                //TODO retry? Feels a bit odd to retry a connect call, but Hub folks do have a prescribed pattern for connect attempts. Maybe offer one connect with retry, one connect w/o
                if (devicePresenceFlowCompletedArgs.Exception != null)
                {
                    throw devicePresenceFlowCompletedArgs.Exception;
                }
            }
            finally
            {
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompleted;
            }
        }

        /// <summary>
        /// Connect to Device Provisioning Service and register this device.
        /// </summary>
        /// <remarks>
        /// The connection layer owns reconnection here just as it does for IoT hub connections. Because DPS cannot
        /// persist sessions, every connection it establishes starts a brand new registration in
        /// <see cref="HandleConnectedToDpsAsync"/>, which is the provisioning counterpart of the device presence flow.
        /// </remarks>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields.</param>
        /// <param name="authentication">The x509 authentication to use when connecting to Device Provisioning Service.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The terminal registration result reported by Device Provisioning Service.</returns>
        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            cancellationToken.ThrowIfCancellationRequested();

            _provisioningRequestPayload = new RegistrationRequestPayload()
            {
                ClientCertificateSigningRequest = null,
                Payload = provisioningSettings.ProvisioningPayload,
            };

            // From here on, every connection this client establishes targets DPS, so every connection (including the ones
            // the connection layer re-establishes on its own) runs the provisioning flow.
            CurrentEndpoint = ConnectionEndpoint.DeviceProvisioningService;

            MqttConnect connect = CreateProvisioningConnectPacket(authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress);

            TaskCompletionSource<ProvisioningFlowCompletedArgs> provisioningFlowResult = new(TaskCreationOptions.RunContinuationsAsynchronously);
            Task HandleProvisioningFlowCompletedAsync(ProvisioningFlowCompletedArgs args)
            {
                provisioningFlowResult.TrySetResult(args);
                return Task.CompletedTask;
            }

            // Setup the callback BEFORE sending CONNECT so that the CONNACK can be handled regardless of how quickly it arrives
            ProvisioningFlowCompletedAsync += HandleProvisioningFlowCompletedAsync;

            try
            {
                // MQTT connection manager already checks connack for non-success cases, so no need to check it here as well
                MqttConnectAck connack = await ManagedMqttConnection.ConnectAsync(connect, cancellationToken);

                ProvisioningFlowCompletedArgs provisioningFlowCompletedArgs = await provisioningFlowResult.Task.WaitAsync(cancellationToken);

                if (provisioningFlowCompletedArgs.Exception != null)
                {
                    throw provisioningFlowCompletedArgs.Exception;
                }

                Debug.Assert(provisioningFlowCompletedArgs.RegistrationResult != null);

                //TODO do we care about initial twin as returned by DPS?
                return provisioningFlowCompletedArgs.RegistrationResult;
            }
            finally
            {
                ProvisioningFlowCompletedAsync -= HandleProvisioningFlowCompletedAsync;

                // Stop any provisioning flow that is still waiting on a DPS response now that no one is listening for its result.
                CancelCurrentProvisioningFlow();
                _provisioningRequestPayload = null;
                CurrentEndpoint = ConnectionEndpoint.None;

                // Always close the MQTT connection once provisioning has finished so that the connection can be
                // re-established against the assigned IoT hub.
                var disconnect = new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection };

                try
                {
                    await ManagedMqttConnection.DisconnectAsync(false, disconnect, CancellationToken.None);
                }
                catch (Exception)
                {
                    // Deliberately not rethrowing the exception because this is a "best effort" close.
                    // The service may not have acknowledged that the client closed the connection, but
                    // all local resources have been closed. The service will eventually realize the
                    // connection is closed in cases like these.
                }
            }
        }

        /// <summary>
        /// Run the provisioning flow upon connecting to Device Provisioning Service, in the same way that
        /// <see cref="HandleConnectedToHubAsync(MqttClientConnectedEventArgs)"/> runs the device presence flow upon
        /// connecting to IoT hub.
        /// </summary>
        /// <remarks>
        /// DPS cannot persist sessions, so the CONNACK's session present flag is deliberately ignored: every connection
        /// starts a brand new registration.
        /// </remarks>
        private async Task HandleConnectedToDpsAsync(MqttClientConnectedEventArgs args)
        {
            // Any flow left over from a previous connection belongs to a session that no longer exists, so abandon it.
            CancelCurrentProvisioningFlow();

            var provisioningFlowCancellation = new CancellationTokenSource();
            _currentProvisioningFlowCancellation = provisioningFlowCancellation;

            // Responses to the previous connection's requests are no longer expected.
            _startProvisioningRequestStatusSource = null;
            _checkRegistrationOperationStatusSource = null;

            try
            {
                await SubscribeToRegistrationResponseMessagesAsync(provisioningFlowCancellation.Token);

                RegistrationOperationStatus registrationStatus = await PublishRegistrationRequestAsync(provisioningFlowCancellation.Token);

                DeviceRegistrationResult registrationResult = await PollUntilProvisioningFinishesAsync(
                    registrationStatus.OperationId,
                    provisioningFlowCancellation.Token);

                if (ProvisioningFlowCompletedAsync != null)
                {
                    await ProvisioningFlowCompletedAsync.Invoke(new ProvisioningFlowCompletedArgs(registrationResult));
                }
            }
            catch (OperationCanceledException)
            {
                // The connection this flow was running on ended, or provisioning was abandoned altogether. If the
                // connection layer re-establishes the connection, this callback starts the flow over from the beginning.
                Trace.TraceWarning("Provisioning flow was abandoned because the connection to DPS ended.");
            }
            catch (Exception e)
            {
                Trace.TraceError("Exception thrown while running the provisioning flow. {0}", e);
                if (ProvisioningFlowCompletedAsync != null)
                {
                    await ProvisioningFlowCompletedAsync.Invoke(new ProvisioningFlowCompletedArgs(e));
                }
            }
            finally
            {
                // Clear the field only if this flow is still the current one, so that a newer flow's cancellation source is left intact.
                Interlocked.CompareExchange(ref _currentProvisioningFlowCancellation, null, provisioningFlowCancellation);
                provisioningFlowCancellation.Dispose();
            }
        }

        private void CancelCurrentProvisioningFlow()
        {
            CancellationTokenSource? provisioningFlowCancellation = Interlocked.Exchange(ref _currentProvisioningFlowCancellation, null);

            try
            {
                provisioningFlowCancellation?.Cancel();
            }
            catch (ObjectDisposedException)
            {
                // The flow already ended and disposed its own cancellation source, so there is nothing left to cancel.
            }
        }

        private async Task SubscribeToRegistrationResponseMessagesAsync(CancellationToken cancellationToken)
        {
            Trace.TraceInformation("Subscribing to DPS response topic {0}", ProvisioningSubscribeFilter);
            MqttSubscribeAck subscribeResults = await ManagedMqttConnection.SubscribeAsync(new(ProvisioningSubscribeFilter, MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken);

            if (subscribeResults.Items.FirstOrDefault()!.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1)
            {
                throw new Exception("todo");
            }
        }

        private async Task<RegistrationOperationStatus> PublishRegistrationRequestAsync(CancellationToken cancellationToken)
        {
            byte[] serializedPayload = Array.Empty<byte>();
            if (_provisioningRequestPayload != null)
            {
                string requestString = JsonSerializer.Serialize(_provisioningRequestPayload, JsonSerializationSettings.Options);
                serializedPayload = Encoding.UTF8.GetBytes(requestString);
            }

            string registrationTopic = string.Format(CultureInfo.InvariantCulture, ProvisioningRegisterTopic, ++_provisioningRequestId);
            MqttPublish publish = new MqttPublish()
            {
                Payload = serializedPayload,
                Topic = registrationTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
            };

            _startProvisioningRequestStatusSource = new TaskCompletionSource<RegistrationOperationStatus>(TaskCreationOptions.RunContinuationsAsynchronously);

            Trace.TraceInformation("Publishing to DPS on topic {0}", registrationTopic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(publish, cancellationToken);

            Trace.TraceInformation("Successfully published registration request to DPS with request Id {0}", _provisioningRequestId);

            try
            {
                RegistrationOperationStatus registrationStatus = await _startProvisioningRequestStatusSource.Task.WaitAsync(cancellationToken);

                return registrationStatus.Status != ProvisioningRegistrationStatus.Assigning
                    ? throw new Exception("TODO")
                    : registrationStatus;
            }
            catch (OperationCanceledException e)
            {
                throw new OperationCanceledException("Timed out waiting for DPS to send the initial provisioning response", e);
            }
        }

        private async Task<DeviceRegistrationResult> PollUntilProvisioningFinishesAsync(string operationId, CancellationToken cancellationToken)
        {
            while (true)
            {
                string topic = string.Format(CultureInfo.InvariantCulture, ProvisioningGetOperationsTopic, ++_provisioningRequestId, operationId);
                MqttPublish message = new MqttPublish()
                {
                    Topic = topic,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
                };

                _checkRegistrationOperationStatusSource = new TaskCompletionSource<RegistrationOperationStatus>(TaskCreationOptions.RunContinuationsAsynchronously);

                Trace.TraceInformation("Publishing to DPS on topic {0}", topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(message, cancellationToken);

                RegistrationOperationStatus currentStatus;
                try
                {
                    currentStatus = await _checkRegistrationOperationStatusSource.Task.WaitAsync(cancellationToken);
                }
                catch (OperationCanceledException e)
                {
                    throw new OperationCanceledException("Timed out waiting for DPS to send a response to the polling request", e);
                }

                Debug.Assert(currentStatus.RegistrationState != null);

                if (currentStatus.RegistrationState.Status != ProvisioningRegistrationStatus.Assigning)
                {
                    // return once a terminal state has been reached
                    return currentStatus.RegistrationState;
                }

                // The service is expected to return a value signalling how long to wait before polling again, but
                // the SDK has a default value for when the service does not send that value. Included in this default value
                // is some jitter to help stagger the requests if multiple provisioning device clients are checking their provisioning
                // state at the same time.
                TimeSpan pollingDelay = currentStatus.RetryAfter ?? RetryJitter.GenerateDelayWithJitterForRetry(s_defaultOperationPollingInterval);

                await Task.Delay(pollingDelay, cancellationToken);
            }
        }

        private MqttConnect CreateProvisioningConnectPacket(X509AuthenticationProvider authentication, string idScope, string globalDeviceEndpoint)
        {
            string hostName = globalDeviceEndpoint;

            string username = string.Format(
                CultureInfo.InvariantCulture,
                ProvisioningUsernameFormat,
                idScope,
                authentication.GetRegistrationId(),
                ProvisioningApiVersion,
                Uri.EscapeDataString(GetProvisioningUserAgentString()));

            return new MqttConnect()
            {
                HostName = hostName,
                TcpPort = 8883,
                WebsocketPort = 443,
                WebsocketUri = $"wss://{hostName}",
                ClientCertificate = authentication.ClientCertificate,
                CleanSession = true, // The DPS MQTT broker does not support session persistence, so setting these clean start/clean session flags does nothing
                CleanStart = true,
                SessionExpiryInterval = 0,
                Username = username,
                Password = Array.Empty<byte>(),
                ClientId = authentication.GetRegistrationId(),
                ProtocolVersion = MqttProtocolVersion.V311
            };
        }

        private Task HandleReceivedProvisioningPublishAsync(MqttPublishReceivedEventArgs receivedEventArgs)
        {
            if (CurrentEndpoint != ConnectionEndpoint.DeviceProvisioningService)
            {
                // This client is not provisioning, so this publish came from IoT hub and is for the feature clients to handle.
                return Task.CompletedTask;
            }

            string topic = receivedEventArgs.Publish.Topic;

            TaskCompletionSource<RegistrationOperationStatus>? startProvisioningRequestStatusSource = _startProvisioningRequestStatusSource;

            if (startProvisioningRequestStatusSource == null)
            {
                // No registration request is outstanding on the current connection, so this publish belongs to a
                // provisioning flow that was abandoned when its connection ended.
                return Task.CompletedTask;
            }

            Trace.TraceInformation("Received MQTT publish from DPS on topic {0}", topic);

            if (!startProvisioningRequestStatusSource.Task.IsCompleted)
            {
                // The initial provisioning request's response topic is shaped like "$dps/registrations/res/202/?$rid=1&retry-after=3"
                string jsonString = Encoding.UTF8.GetString(receivedEventArgs.Publish.Payload);
                RegistrationOperationStatus operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(jsonString, JsonSerializationSettings.Options)!;
                startProvisioningRequestStatusSource.TrySetResult(operation);
            }
            else
            {
                TaskCompletionSource<RegistrationOperationStatus>? checkRegistrationOperationStatusSource = _checkRegistrationOperationStatusSource;

                if (checkRegistrationOperationStatusSource == null)
                {
                    // No polling request is outstanding, so there is nothing waiting on this response.
                    return Task.CompletedTask;
                }

                // All status polling requests' response topics are shaped like "$dps/registrations/res/200/?$rid=2"
                string jsonString = Encoding.UTF8.GetString(receivedEventArgs.Publish.Payload);
                RegistrationOperationStatus operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(jsonString, JsonSerializationSettings.Options)!;
                operation.RetryAfter = GetRetryAfterFromTopic(topic, s_defaultOperationPollingInterval);

                checkRegistrationOperationStatusSource.TrySetResult(operation);
            }

            return Task.CompletedTask;
        }

        private static TimeSpan? GetRetryAfterFromTopic(string topic, TimeSpan defaultPoolingInterval)
        {
            string[] topicAndQueryString = topic.Split('?');
            if (topicAndQueryString.Length > 1)
            {
                string[] queryPairs = topicAndQueryString[1].Split('&');
                for (int queryPairIndex = 0; queryPairIndex < queryPairs.Length; queryPairIndex++)
                {
                    string[] queryKeyAndValue = queryPairs[queryPairIndex].Split('=');
                    if (queryKeyAndValue.Length == 2 && queryKeyAndValue[0].Equals(RetryAfterHeader, StringComparison.OrdinalIgnoreCase))
                    {
                        if (int.TryParse(queryKeyAndValue[1], out int secondsToWait))
                        {
                            var serviceRecommendedDelay = TimeSpan.FromSeconds(secondsToWait);

                            return serviceRecommendedDelay.TotalSeconds < defaultPoolingInterval.TotalSeconds
                                ? defaultPoolingInterval
                                : serviceRecommendedDelay;
                        }
                    }
                }
            }

            return null;
        }

        private static string GetProvisioningUserAgentString()
        {
            const string name = "Microsoft.Azure.Devices.Provisioning.Client";

            string version = typeof(AbstractConnectionClient).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            string userAgent = $"{name}/{version} ({runtime}; {operatingSystem}; {processorArchitecture})";

            return userAgent;
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public virtual void Dispose(bool disposing)
        {
            DetachConnectionCallbacks();

            if (disposing)
            {
                ManagedMqttConnection.Dispose();
            }
            else if (!_isUserSuppliedMqttClient)
            {
                ManagedMqttConnection.Dispose();
            }

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public virtual void Dispose()
        {
            DetachConnectionCallbacks();

            ManagedMqttConnection.Dispose();

            _isDisposed = true;
        }

        private void DetachConnectionCallbacks()
        {
            ManagedMqttConnection.ConnectingAsync -= PatchConnectPacketAsync;
            ManagedMqttConnection.ConnectedAsync -= HandleConnectedAsync;
            ManagedMqttConnection.DisconnectedAsync -= HandleDisconnectedAsync;
            ManagedMqttConnection.ConnectionFaultedAsync -= HandleConnectionFaultedAsync;
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedProvisioningPublishAsync;
            ManagedMqttConnection.PublishReceivedAsync -= DelegatePublishAsync;

            CancelCurrentProvisioningFlow();
            CurrentEndpoint = ConnectionEndpoint.None;
        }

        private async Task<TResp> PerformWhileRespectingConnectionState<TResp>(Func<CancellationToken, Task<TResp>> funcToRetry, CancellationToken cancellationToken)
        {
            // Assume an open connection to start, but increment this by one if MqttClientNotConnectedException is thrown to counteract that assumption
            using ManualResetEventSlim latch = new();
            latch.Set();
            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompleted = (args) =>
            {
                latch.Set();
                return Task.CompletedTask;
            };

            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompleted;
            try
            {
                while (true) // Retry sending publish until user cancels as long as the failure is just that the underlying mqtt client was disconnected.
                {
                    try
                    {
                        return await funcToRetry.Invoke(cancellationToken);
                    }
                    catch (MqttClientNotConnectedException)
                    {
                        latch.Reset(); // No-op if the HandleDisconnection already reset this latch. Only here because there is a chance that this method was called while MQTT client was disconnected

                        try
                        {
                            latch.Wait(cancellationToken); // Wait for device birth flow to finish before resuming this feature client-level traffic
                        }
                        catch (OperationCanceledException)
                        {
                            throw new OperationCanceledException("Operation canceled while waiting for reconnection to finish");
                        }
                    }
                }
            }
            finally
            {
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompleted;
            }
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttPublishAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.PublishAsync(publish, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttSubscribeAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.SubscribeAsync(subscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttUnsubscribeAck>> funcToRetry = async (args) =>
            {
                return await ManagedMqttConnection.UnsubscribeAsync(unsubscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }
    }
}
