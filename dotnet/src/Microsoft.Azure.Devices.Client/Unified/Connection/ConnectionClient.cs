using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session;
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

        private IMqttClient _mqttClient;

        private ConnectionContext? CurrentConnectionContext { get; set; }

        public IMqttClient MqttClient => _mqttClient;

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        /// <summary>
        /// This event signals that the device is connected and has established all necessary subscriptions with IoT Hub.
        /// </summary>
        public event Action? DeviceReadyAsync; //TODO Ideally, user wouldn't even have to care about this and it could be private. Just use it to co-ordinate locally around when to send user traffic during/after disconnection handling
        
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

            MqttSessionClientOptions sessionClientOptions = new()
            {
                ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                EnableMqttLogging = options.EnableMqttLogging,
            };

            _isUserSuppliedMqttClient = options.MqttClient != null;

            _mqttClient = options.MqttClient ?? new MqttSessionClient(sessionClientOptions);

            _gen2ConnectionClient = new(options);

            _mqttClient.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;

            _mqttClient.PublishReceivedAsync += DelegatePublishAsync; // relay all publishes from the underlying MQTT client to users of this connection client
        }

        private async Task DelegatePublishAsync(MqttPublishReceivedEventArgs args)
        {
            if (PublishReceivedAsync != null)
            {
                await PublishReceivedAsync.Invoke(args);
            }
        }

        private async Task HandleGen1IotHubConnectionAsync(MqttClientConnectedEventArgs args)
        {
            // Note that this callback handler is only set after provisioning has completed. It is only to respond to IoT Hub connection events, not DPS connection events.

            // Upon an MQTT connection being established, immediately re-subscribe to all twin/telemetry/direct methods topics if there is no session present.
            if (args.ConnectAck.IsSessionPresent)
            {
                return; //TODO gen2 client case?
            }

            // Run task in background b/c it uses the MQTT client to send a SUBSCRIBE which requires we release this thread
            _ = Task.Run(async () =>
            {
                // This callback should only be reached after provisioning, so their should always be a connection context to use
                Debug.Assert(CurrentConnectionContext != null);

                //TODO check for previous connack isSessionPresent flag before firing off all these subscriptions?
                MqttSubscribe mqttSubscribe = new();
                var expectedQos = MqttQualityOfServiceLevel.AtMostOnce;
                mqttSubscribe.TopicFilters.Add(new(string.Format(Telemetry.TelemetryClient.DeviceBoundMessagesTopicFormat + "#", CurrentConnectionContext.DeviceId), expectedQos));
                mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinResponseTopic + "#", expectedQos));
                mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#", expectedQos));
                mqttSubscribe.TopicFilters.Add(new(DirectMethods.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#", expectedQos));
                var suback = await _mqttClient.SubscribeAsync(mqttSubscribe);

                bool anySubscribeFailed = false;
                foreach (var topicSuback in suback.Items)
                {
                    anySubscribeFailed |= (topicSuback.ResultCode != MqttClientSubscribeResultCode.GrantedQoS0);
                }

                //TODO how to signal that the connect needs to throw?
                if (anySubscribeFailed)
                {
                    await _mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.ImplementationSpecificError }); //Does the session client stop retrying here? Use a disconnect code to signal
                }
                else
                {
                    if (DeviceReadyAsync != null)
                    { 
                        DeviceReadyAsync.Invoke();
                    }
                }
            });
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

            _mqttClient.ConnectedAsync -= HandleGen1IotHubConnectionAsync; //TODO be very careful about when we register/unregister this callback expecially around connection loss scenarios

            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            //TODO several mqtt client options should not be provided by the user (ie, host name). Add checks here that validate all of them

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId!,
                IotHubHostName = provisioningResult.AssignedHub!,
                IsGen2Hub = provisioningResult.IsAzureEventGridHub,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                AuthenticationProvider = authentication,
            };

            _mqttClient.ConnectedAsync += HandleGen1IotHubConnectionAsync;

            // After successful provisioning, connect using the appropriate logic based on the Hub this device was provisioned to
            //TODO use issued client certs if CSR was done during provisioning
            if (provisioningResult.IsAzureEventGridHub)
            {
                await _gen2ConnectionClient.ConnectAsync(
                    new ConnectionContext()
                    {
                        DeviceId = provisioningResult.DeviceId!,
                        IotHubHostName = provisioningResult.AssignedHub!,
                        IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,
                        AuthenticationProvider = authentication,
                        IsGen2Hub = true,
                    },
                    null,
                    cancellationToken);
            }
            else
            {
                await ConnectAsync(CurrentConnectionContext, cancellationToken);
            }

            return CurrentConnectionContext;
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            await _mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection }, cancellationToken);
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

                await _mqttClient.SubscribeAsync(new(CertificateSigningResponseTopicFilter, MqttQualityOfServiceLevel.AtLeastOnce)); // TODO QoS correct?

                MqttPublish certificateSigningRequestPublish = new()
                {
                    Topic = CertificateSigningRequestTopic + request.RequestId,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(request),
                };

                MqttPublishAck puback = await _mqttClient.PublishAsync(certificateSigningRequestPublish, cancellationToken: cancellationToken);

                PublishRejectedException.ThrowIfUnsuccessfulPuback(puback, "Failed to send the certificate signing request because the MQTT broker rejected the publish.");
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

            var connack = await _mqttClient.ConnectAsync(connectPacket, cancellationToken);
            ConnectRejectedException.ThrowIfUnsuccessfulConnack(connack, "Connection to IoT Hub was rejected.");

            TaskCompletionSource OnSubscribedTcs = new();
            this.DeviceReadyAsync += async () =>
            {
                OnSubscribedTcs.TrySetResult();
            };

            await OnSubscribedTcs.Task.WaitAsync(cancellationToken);
        }

        internal async Task<DeviceRegistrationResult> ProvisionAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            ProvisioningConnection provisioningConnection = new();
            return await provisioningConnection.RegisterAsync(_mqttClient, new() { ClientCertificateSigningRequest = null, Payload = provisioningSettings.ProvisioningPayload }, authentication, provisioningSettings.IdScope, provisioningSettings.GlobalEndpointAddress, cancellationToken);

            //TODO do we care about initial twin as returned by DPS?
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
            _mqttClient.PublishReceivedAsync -= DelegatePublishAsync;
            _mqttClient.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            _mqttClient.ConnectedAsync -= HandleGen1IotHubConnectionAsync;

            if (disposing)
            {
                _mqttClient.Dispose();
            }
            else if (!_isUserSuppliedMqttClient)
            {
                _mqttClient.Dispose();
            }

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _mqttClient.PublishReceivedAsync -= PublishReceivedAsync;
            _mqttClient.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            _mqttClient.ConnectedAsync -= HandleGen1IotHubConnectionAsync;
            _mqttClient.Dispose();

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

        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            //TODO to achieve a sort of "pause" on user-traffic when a reconnection happens, I could cancel this request (upon disconnect) down to the session client and then
            // re-submit it after connection has been re-established
            return _mqttClient.PublishAsync(publish, cancellationToken);
        }

        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            // same as publish
            return _mqttClient.SubscribeAsync(subscribe, cancellationToken);
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            // same as publish
            return _mqttClient.UnsubscribeAsync(unsubscribe, cancellationToken);
        }
    }
}
