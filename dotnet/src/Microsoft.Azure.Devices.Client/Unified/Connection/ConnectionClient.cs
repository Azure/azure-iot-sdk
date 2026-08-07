using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session;
using Microsoft.Azure.Devices.Client.Provisioning;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Collections.Concurrent;
using System.ComponentModel;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    public class ConnectionClient : IConnectionClient
    {
        private IMqttClient _mqttClient;

        private ConnectionContext? CurrentConnectionContext { get; set; }

        public IMqttClient MqttClient => _mqttClient;

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        private const string CertificateSigningRequestTopic = "$iothub/credentials/POST/issueCertificate/?$rid=";
        private const string CertificateSigningResponseTopicFilter = "$iothub/credentials/res/#";
        private const string CertificateSigningResponseTopic = "$iothub/credentials/res/";

        private const string RequestId = "?$rid=";

        private readonly ConcurrentDictionary<string, CertificateSigningOperation> _pendingCertificateSigningOperations = new();

        /// <inheritdoc/>
        [EditorBrowsable(EditorBrowsableState.Advanced)]
        public event Func<MqttPublishReceivedEventArgs, Task>? ApplicationMessageReceivedAsync;

        /// <inheritdoc/>
        public event Action<MqttClientConnectedEventArgs>? ConnectedAsync;

        /// <inheritdoc/>
        public event Action<MqttClientDisconnectedEventArgs>? DisconnectedAsync;

        private Gen2.Connection.ConnectionClient _genConnectionClient;

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        public ConnectionClient(ConnectionClientOptions options)
        {
            options ??= new ConnectionClientOptions();

            MqttSessionClientOptions sessionClientOptions = new()
            {
                ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                EnableMqttLogging = options.EnableMqttLogging,
            };

            _mqttClient = options.MqttClient ?? new MqttSessionClient(sessionClientOptions);

            _genConnectionClient = new(options);

            _mqttClient.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;
        }

        /// <summary>
        /// Empty constructor mostly for mocking purposes
        /// </summary>
        public ConnectionClient()
        {
            _mqttClient = new MqttSessionClient(new());
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
            var provisioningResult = await ProvisionAsync(provisioningSettings, authentication, cancellationToken);

            //TODO several mqtt client options should not be provided by the user (ie, host name). Add checks here that validate all of them

            CurrentConnectionContext = new ConnectionContext()
            {
                DeviceId = provisioningResult.DeviceId,
                IotHubHostName = provisioningResult.AssignedHub,
                IsAzureEventGrid = provisioningResult.IsAzureEventGridHub,
                IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain,                
            };

            // After successful provisioning, connect using the appropriate logic based on the Hub this device was provisioned to
            //TODO use issued client certs if CSR was done during provisioning
            if (provisioningResult.IsAzureEventGridHub)
            {
                await _genConnectionClient.ConnectAsync(
                    new Gen2.Connection.ConnectionContext()
                    {
                        DeviceId = provisioningResult.DeviceId,
                        IsAzureEventGrid = provisioningResult.IsAzureEventGridHub,
                        IotHubHostName = provisioningResult.AssignedHub,
                        IssuedClientCertificates = provisioningResult.IssuedClientCertificateChain
                    },
                    authentication,
                    null,
                    cancellationToken);
            }
            else
            {
                await ConnectAsync(CurrentConnectionContext, authentication, cancellationToken);
            }

            return CurrentConnectionContext;
        }

        /// <summary>
        /// Disconnect this device from IoT hub.
        /// </summary>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task DisconnectAsync(CancellationToken cancellationToken = default)
        {
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
            if (CurrentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            CertificateSigningOperation operation = new();


            if (CurrentConnectionContext.IsAzureEventGrid)
            {
                return await _genConnectionClient.SendCertificateSigningRequestAsync(request, cancellationToken);
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
        /// <param name="authentication">The authentication to use when connecting.</param>
        /// <param name="cancellationToken">Cancellation token.</param>
        /// <returns>The initial twin of the device if a twin push was configured via <see cref="TwinPushOptions"/></returns>
        public async Task ConnectAsync(ConnectionContext connectionContext, X509AuthenticationProvider authentication, CancellationToken cancellationToken = default)
        {
            CurrentConnectionContext = connectionContext;

            if (connectionContext.IsAzureEventGrid)
            {
                // Connect to the new Azure Event Grid endpoint using MQTT v5 using the provisioning result credentials
                await IotHubConnection.ConnectToAzureEventGridIotHubAsync(_mqttClient, connectionContext.IotHubHostName, connectionContext.DeviceId, authentication, cancellationToken);
            }
            else
            {
                // Connect to the legacy IoT hub endpoint using MQTT v3 using the provisioning result credentials
                await IotHubConnection.ConnectToClassicIotHubAsync(_mqttClient, connectionContext.IotHubHostName, connectionContext.DeviceId, authentication, cancellationToken);
            }
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

        public void Dispose()
        {
            _mqttClient.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            _mqttClient.Dispose();
        }
    }
}
