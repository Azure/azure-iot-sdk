using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.Provisioning;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    public class ConnectionClient : AbstractConnectionClient, IConnectionClient
    {
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
        public ConnectionClient(ConnectionClientOptions? options = null) : base(options)
        {
            options ??= new ConnectionClientOptions();

            _gen2ConnectionClient = new(options);

            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;

            _gen2ConnectionClient.DevicePresenceFlowCompletedAsync += HandleGen2ClientConnectionReady;
        }

        private async Task HandleGen2ClientConnectionReady(DevicePresenceFlowCompletedArgs args)
        {
            // When the underlying gen2 connection client has re-established its presence, then it is ready to use (analogous to a gen1 client that has finished re-subscribing to twin/direct methods/telemetry topics)
            await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
        }

        public override MqttConnect MqttConnectOverride(MqttConnect connect)
        {
            //Defer to gen 2 client
            Debug.Assert(CurrentConnectionContext != null);
            if (CurrentConnectionContext.IsGen2Hub)
            {
                return _gen2ConnectionClient.MqttConnectOverride(connect);
            }

            connect.WebsocketUri = $"wss://{connect.HostName}/$iothub/websocket";
            connect.ProtocolVersion = CurrentConnectionContext.IsGen2Hub ? MqttProtocolVersion.V500 : MqttProtocolVersion.V311;
            connect.Username = $"{connect.HostName}/{connect.ClientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";
            connect.Password = Array.Empty<byte>();


            // gen1 flow does not need to insert anything unique per connect attempt
            return connect;
        }

        public override async Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            // Note that this callback handler is only set after provisioning has completed. It is only to respond to IoT Hub connection events, not DPS connection events.

            // This callback should only be reached after provisioning, so their should always be a connection context to use
            Debug.Assert(CurrentConnectionContext != null);

            if (CurrentConnectionContext.IsGen2Hub)
            {
                // A gen 2 hub connection re-announces this device's birth instead of re-subscribing to the classic topics.
                // The gen 2 client owns that flow, but it must run on this client's connection. It signals that the device
                // is ready by raising its own device presence flow completed event, which this client relays.
                await _gen2ConnectionClient.AnnounceDevicePresenceAsync(ManagedMqttConnection, CurrentConnectionContext.DeviceId, args);
                return;
            }

            // Upon an MQTT connection being established, immediately re-subscribe to all twin/telemetry/direct methods topics if there is no session present.
            if (args.ConnectAck.IsSessionPresent)
            {
                await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });

                return;
            }

            //TODO check for previous connack isSessionPresent flag before firing off all these subscriptions?
            MqttSubscribe mqttSubscribe = new();
            var expectedQos = MqttQualityOfServiceLevel.AtMostOnce;
            mqttSubscribe.TopicFilters.Add(new(string.Format(Telemetry.TelemetryClient.DeviceBoundMessagesTopicFormat + "#", CurrentConnectionContext.DeviceId), expectedQos));
            mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinResponseTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(Twin.TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(DirectMethods.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#", expectedQos));
            var suback = await ManagedMqttConnection.SubscribeAsync(mqttSubscribe);

            bool anySubscribeFailed = false;
            foreach (var topicSuback in suback.Items)
            {
                anySubscribeFailed |= (topicSuback.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS0);
            }

            if (anySubscribeFailed)
            {
                // Signal to the underlying MQTT connection manager that, even though we are manually disconnecting, we still want to reconnect.
                await ManagedMqttConnection.DisconnectAsync(true, new MqttDisconnect());

                Trace.TraceError("Device failed to subscribe to one or more necessary MQTT topics upon connecting to IoT Hub. Disconnecting the MQTT client and trying again.");
            }
            else
            {
                await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
            }
        }

        /// <summary>
        /// Send a certificate signing request to IoT hub
        /// </summary>
        /// <param name="request">The certificates to have IoT hub sign.</param>
        /// <param name="cancellationToken">The cancellation token</param>
        /// <returns>A set of tasks. One that completes when IoT hub accepts the request (and starts signing), one that completes when IoT hub completes the signing, and one that completes if any step in the process fails.</returns>
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(IotHubCertificateSigningRequest request, CancellationToken cancellationToken = default)
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

                await ManagedMqttConnection.SubscribeAsync(new(CertificateSigningResponseTopicFilter, MqttQualityOfServiceLevel.AtLeastOnce)); // TODO QoS correct?

                MqttPublish certificateSigningRequestPublish = new()
                {
                    Topic = CertificateSigningRequestTopic + request.RequestId,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(request),
                };

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(certificateSigningRequestPublish, cancellationToken: cancellationToken);
            }

            return operation;
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
                    if (HandleCertificateSigningCompleteAsync != null)
                    {
                        //TODO need a fault-injection like unit test that ensures that the client uses this new authentication provider upon reconnect since our API won't allow users to disconnect then reconnect to hub at will
                        Debug.Assert(CurrentConnectionContext != null);
                        CurrentConnectionContext.AuthenticationProvider = await HandleCertificateSigningCompleteAsync(response.Certificates);
                    }
                    else
                    {
                        Trace.TraceError("Certificate signing response could not update authentication provider because user never set \"HandleCertificateSigningCompleteAsync\" callback");
                    }
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
        public override void Dispose(bool disposing)
        {
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            base.Dispose(disposing);
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public override void Dispose()
        {
            ManagedMqttConnection.PublishReceivedAsync -= HandleReceivedCertificateSigningPublish;
            base.Dispose();
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
    }
}
