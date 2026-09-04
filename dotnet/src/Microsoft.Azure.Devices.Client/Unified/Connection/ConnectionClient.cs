using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    public class ConnectionClient : IConnectionClient
    {
        private bool _isDisposed = false;
        private bool _isUserSuppliedMqttClient = false;

        private MqttConnectionManager _managedMqttConnection;

        private ConnectionContext? CurrentConnectionContext { get; set; }

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        /// <summary>
        /// This event signals that the device is connected and has established all necessary subscriptions with IoT Hub.
        /// </summary>
        private event Func<DevicePresenceFlowCompletedArgs, Task>? DevicePresenceFlowCompletedAsync;

        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        private const string CertificateSigningRequestTopic = "$iothub/credentials/POST/issueCertificate/?$rid=";
        private const string CertificateSigningResponseTopicFilter = "$iothub/credentials/res/#";
        private const string CertificateSigningResponseTopic = "$iothub/credentials/res/";
        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        private const string RequestId = "?$rid=";

        private readonly ConcurrentDictionary<string, CertificateSigningOperation> _pendingCertificateSigningOperations = new();

        private Gen2.Connection.ConnectionClient _gen2ConnectionClient;

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        public ConnectionClient(ConnectionClientOptions? options = null)
        {
            options ??= new ConnectionClientOptions();

            _isUserSuppliedMqttClient = options.MqttClient != null;

            // This is the basic MQTT client that has no reconnection/retry logic
            var unmanagedMqttClient = options.MqttClient ?? new MqttNetClient(enableMqttLogs: options.EnableMqttLogging);

            // This is the wrapper that manages reconnection
            _managedMqttConnection = new(unmanagedMqttClient, options.ConnectionAttemptTimeout, options.ConnectionRetryPolicy);

            _gen2ConnectionClient = new(options);

            _managedMqttConnection.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;

            _managedMqttConnection.PublishReceivedAsync += DelegatePublishAsync; // relay all publishes from the underlying MQTT client to users of this connection client

            _gen2ConnectionClient.DevicePresenceFlowCompletedAsync += HandleGen2ClientConnectionReady;
        }

        private async Task HandleGen2ClientConnectionReady(DevicePresenceFlowCompletedArgs args)
        {
            // When the underlying gen2 connection client has re-established its presence, then it is ready to use (analogous to a gen1 client that has finished re-subscribing to twin/direct methods/telemetry topics)
            if (DevicePresenceFlowCompletedAsync != null)
            {
                await DevicePresenceFlowCompletedAsync.Invoke(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
            }
        }

        private async Task DelegatePublishAsync(MqttPublishReceivedEventArgs args)
        {
            if (PublishReceivedAsync != null)
            {
                await PublishReceivedAsync.Invoke(args);
            }
        }

        private async Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            // Note that this callback handler is only set after provisioning has completed. It is only to respond to IoT Hub connection events, not DPS connection events.

            // Upon an MQTT connection being established, immediately re-subscribe to all twin/telemetry/direct methods topics if there is no session present.
            if (args.ConnectAck.IsSessionPresent)
            {
                Debug.Assert(CurrentConnectionContext != null); // The context should be set even before the first connect attempt, so this should never fail
                if (DevicePresenceFlowCompletedAsync != null && !CurrentConnectionContext.IsGen2Hub)
                {
                    // Only signal device is ready here for gen 1 client case. Gen 2 client still needs to re-announce birth before it is ready to resume normal traffic
                    await DevicePresenceFlowCompletedAsync.Invoke(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
                }

                return;
            }

            // This callback should only be reached after provisioning, so their should always be a connection context to use
            Debug.Assert(CurrentConnectionContext != null);

            //TODO check for previous connack isSessionPresent flag before firing off all these subscriptions?
            MqttSubscribe mqttSubscribe = new();
            var expectedQos = MqttQualityOfServiceLevel.AtMostOnce;
            mqttSubscribe.TopicFilters.Add(new(string.Format(Telemetry.TelemetryClient.DeviceBoundMessagesTopicFormat + "#", CurrentConnectionContext.DeviceId), expectedQos));
            mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinResponseTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(DirectMethods.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#", expectedQos));
            var suback = await _managedMqttConnection.SubscribeAsync(mqttSubscribe);

            bool anySubscribeFailed = false;
            foreach (var topicSuback in suback.Items)
            {
                anySubscribeFailed |= (topicSuback.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS0);
            }

            if (anySubscribeFailed)
            {
                // Signal to the underlying MQTT connection manager that, even though we are manually disconnecting, we still want to reconnect.
                await _managedMqttConnection.DisconnectAsync(true, new MqttDisconnect());

                Trace.TraceError("Device failed to subscribe to one or more necessary MQTT topics upon connecting to IoT Hub. Disconnecting the MQTT client and trying again.");
            }
            else
            {
                if (DevicePresenceFlowCompletedAsync != null)
                {
                    await DevicePresenceFlowCompletedAsync.Invoke(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
                }
            }
        }

        /// <summary>
        /// Provision this device with the provided credentials using Device Provisioning Service, then connect this device to the IoT hub it was provisioned to.
        /// </summary>
        /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields</param>
        /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>The received twin push upon connecting to IoT hub if any part of the twin was configured to be pushed in <see cref="TwinPushOptions"/>.</returns>
        public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync; // Don't respond to connection attempts to DPS with IoT hub connection handling

            ProvisioningConnection provisioningConnection = new();
            var provisioningResult = await provisioningConnection.RegisterAsync(_managedMqttConnection, new() { ClientCertificateSigningRequest = null, Payload = provisioningSettings.ProvisioningPayload }, authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress, cancellationToken);

            //TODO several mqtt client options should not be provided by the user (ie, host name). Add checks here that validate all of them

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId!,
                IotHubHostName = provisioningResult.AssignedHub!,
                IsGen2Hub = provisioningResult.IsAzureEventGridHub,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                AuthenticationProvider = authentication,
            };

            _managedMqttConnection.ConnectedAsync += HandleConnectedToHubAsync;

            //TODO use issued client certs if CSR was done during provisioning
            await ConnectAsync(CurrentConnectionContext, cancellationToken);

            return CurrentConnectionContext;
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            await _managedMqttConnection.DisconnectAsync(false, new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection }, cancellationToken);
            CurrentConnectionContext = null;
        }

        /// <summary>
        /// Send a certificate signing request to IoT hub
        /// </summary>
        /// <param name="request">The certificates to have IoT hub sign.</param>
        /// <param name="cancellationToken">The cancellation token</param>
        /// <returns>A set of tasks. One that completes when IoT hub accepts the request (and starts signing), one that completes when IoT hub completes the signing, and one that completes if any step in the process fails.</returns>
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(CertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            if (CurrentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            CertificateSigningOperation operation = new();


            if (CurrentConnectionContext.IsGen2Hub)
            {
                return await _gen2ConnectionClient.SendCertificateSigningRequestAsync(request, cancellationToken);
            }
            else
            {
                _pendingCertificateSigningOperations.TryAdd(request.RequestId, operation);

                await _managedMqttConnection.SubscribeAsync(new(CertificateSigningResponseTopicFilter, MqttQualityOfServiceLevel.AtLeastOnce)); // TODO QoS correct?

                MqttPublish certificateSigningRequestPublish = new()
                {
                    Topic = CertificateSigningRequestTopic + request.RequestId,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(request),
                };

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await _managedMqttConnection.PublishAsync(certificateSigningRequestPublish, cancellationToken: cancellationToken);
            }

            return operation;
        }

        /// <summary>
        /// Connect directly to IoT Hub
        /// </summary>
        /// <param name="connectionContext">The details about which IoT hub host to connect to, and which device Id to connect as.</param>
        /// <param name="cancellationToken">Cancellation token.</param>
        public async Task ConnectAsync(ConnectionContext connectionContext, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            CurrentConnectionContext = connectionContext;

            if (connectionContext.IsGen2Hub)
            {
                ConnectionContext gen2Context = new()
                {
                    AuthenticationProvider = CurrentConnectionContext.AuthenticationProvider,
                    DeviceId = CurrentConnectionContext.DeviceId,
                    IotHubHostName = CurrentConnectionContext.IotHubHostName,
                    IssuedClientCertificates = CurrentConnectionContext.IssuedClientCertificates,
                    IsGen2Hub = true,
                };

                // Connect to the new Azure Event Grid endpoint using MQTT v5 using the provisioning result credentials
                await _gen2ConnectionClient.ConnectAsync(gen2Context, null, cancellationToken);
                return;
            }

            _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;
            _managedMqttConnection.ConnectedAsync += HandleConnectedToHubAsync;

            string clientId = connectionContext.DeviceId;
            string deviceId = connectionContext.DeviceId;
            string hostname = connectionContext.IotHubHostName;

            //TODO what is the latest Hub API version?
            string username = $"{hostname}/{clientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";

            MqttConnect connectPacket = new MqttConnect()
            {
                HostName = hostname,
                TcpPort = 8883,
                WebsocketPort = 443,
                WebsocketUri = $"wss://{hostname}/$iothub/websocket",
                ClientCertificate = connectionContext.AuthenticationProvider.ClientCertificate,
                CleanSession = false, //TODO user configurable value?
                Username = username,
                Password = Array.Empty<byte>(),
                ClientId = clientId,
                ProtocolVersion = MqttProtocolVersion.V311
            };

            // Setup callbacks BEFORE sending CONNECT so that CONNACK can be handled regardless of how quickly it arrives
            TaskCompletionSource OnSubscribedTcs = new();
            this.DevicePresenceFlowCompletedAsync += async (args) =>
            {
                if (args.IsSuccess)
                {
                    OnSubscribedTcs.TrySetResult();
                }
                else if (args.Exception != null)
                {
                    OnSubscribedTcs.TrySetException(args.Exception);
                }
            };

            var connack = await _managedMqttConnection.ConnectAsync(connectPacket, cancellationToken);
            ConnectRejectedException.ThrowIfUnsuccessfulConnack(connack, "Connection to IoT Hub was rejected.");

            await OnSubscribedTcs.Task.WaitAsync(cancellationToken);
        }

        private async Task HandleReceivedCertificateSigningPublish(MqttPublishReceivedEventArgs args)
        {
            if (args.Publish.Topic.StartsWith(CertificateSigningResponseTopic))
            {
                string[] topicTokens = args.Publish.Topic.Split("/");
                if (topicTokens.Length != 5)
                {
                    return;
                }

                string status = topicTokens[3];
                string requestId = topicTokens[4].Split(RequestId)[1];

                if (!_pendingCertificateSigningOperations.TryGetValue(requestId, out var pendingCertificateSigningOperation))
                {
                    return;
                }

                if (status.Equals("202"))
                {
                    CertificateSigningRequestAccepted accepted = JsonSerializer.Deserialize<CertificateSigningRequestAccepted>(args.Publish.Payload)!;
                    pendingCertificateSigningOperation.SetAccepted(accepted);
                    //TODO qos? Ack needed?
                    return;
                }
                else if (status.Equals("200"))
                {
                    CertificateSigningResponse response = JsonSerializer.Deserialize<CertificateSigningResponse>(args.Publish.Payload)!;
                    pendingCertificateSigningOperation.SetCompleted(response);
                    //TODO qos? Ack needed?
                    return;
                }
                else
                {
                    CertificateSigningRequestErrorResponse error = JsonSerializer.Deserialize<CertificateSigningRequestErrorResponse>(args.Publish.Payload)!;
                    pendingCertificateSigningOperation.SetFailed(new CertificateSigningRequestFailedException() { Error = error });
                    //TODO qos? Ack needed?
                    return;
                }
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _managedMqttConnection.PublishReceivedAsync -= DelegatePublishAsync;
            _managedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;

            if (disposing)
            {
                _managedMqttConnection.Dispose();
            }
            else if (!_isUserSuppliedMqttClient)
            {
                _managedMqttConnection.Dispose();
            }

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _managedMqttConnection.PublishReceivedAsync -= PublishReceivedAsync;
            _managedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            _managedMqttConnection.ConnectedAsync -= HandleConnectedToHubAsync;
            _managedMqttConnection.Dispose();

            _isDisposed = true;
        }

        private static string GetUserAgentString()
        {
            const string name = "Microsoft.Azure.Devices.Client";

            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            string userAgent = $"{name}/{GetPackageVersion()} ({runtime}; {operatingSystem}; {processorArchitecture})";

            return userAgent;
        }

        private static string GetPackageVersion()
        {
            return typeof(ConnectionClient).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }

        private async Task<TResp> PerformWhileRespectingConnectionState<TResp>(Func<CancellationToken, Task<TResp>> funcToRetry, CancellationToken cancellationToken)
        {
            // Assume an open connection to start, but increment this by one if MqttClientNotConnectedException is thrown to counteract that assumption
            using ManualResetEventSlim latch = new();
            latch.Set();

            Func<DevicePresenceFlowCompletedArgs, Task> HandleDevicePresenceFlowCompletedAsync = async (args) =>
            {
                if (args.IsSuccess)
                {
                    latch.Set();
                }
            };

            DevicePresenceFlowCompletedAsync += HandleDevicePresenceFlowCompletedAsync;
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
                            latch.Wait(cancellationToken);
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
                DevicePresenceFlowCompletedAsync -= HandleDevicePresenceFlowCompletedAsync;
            }
        }

        /// <summary>
        /// 
        /// </summary>
        /// <param name="publish"></param>
        /// <param name="cancellationToken"></param>
        /// <exception cref="DeviceException">TODO document how this is passed back to the feature clients</exception>
        /// <returns></returns>
        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttPublishAck>> funcToRetry = async (args) =>
            {
                return await _managedMqttConnection.PublishAsync(publish, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttSubscribeAck>> funcToRetry = async (args) =>
            {
                return await _managedMqttConnection.SubscribeAsync(subscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            Func<CancellationToken, Task<MqttUnsubscribeAck>> funcToRetry = async (args) =>
            {
                return await _managedMqttConnection.UnsubscribeAsync(unsubscribe, cancellationToken);
            };

            return await PerformWhileRespectingConnectionState(funcToRetry, cancellationToken);
        }
    }
}
