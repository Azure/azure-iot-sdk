// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTnetAdapter;
using Microsoft.Azure.Iot.Device.Provisioning;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Authentication;

namespace Microsoft.Azure.Iot.Device.Unified.Connection
{
    public class ConnectionClient : AbstractConnectionClient, IConnectionClient
    {
        private const string CertificateSigningRequestTopic = "$iothub/credentials/POST/issueCertificate/?$rid=";
        private const string CertificateSigningResponseTopicFilter = "$iothub/credentials/res/#";
        private const string CertificateSigningResponseTopic = "$iothub/credentials/res/";
        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        private const string RequestId = "?$rid=";

        private readonly ConcurrentDictionary<string, CertificateSigningOperation> _pendingCertificateSigningOperations = new();

        /// <summary>
        /// Construct a new <see cref="ConnectionClient"/>
        /// </summary>
        /// <param name="options">
        /// The optional configurations that this client will use
        /// </param>
        /// <param name="connectionContext">
        /// An optional IoT hub assignment the application restored (for example, one persisted to disk across a device
        /// reboot). When supplied, <see cref="AbstractConnectionClient.ProvisionAndConnectAsync"/> attempts to connect
        /// directly to this IoT hub before provisioning for a new assignment.
        /// </param>
        public ConnectionClient(ConnectionClientOptions? options = null, ConnectionContext? connectionContext = null) : base(options, connectionContext)
        {
            options ??= new ConnectionClientOptions();

            ManagedMqttConnection.PublishReceivedAsync += HandleReceivedCertificateSigningPublish;
        }

        private async Task HandleMQTTv5ClientConnectionReady(DevicePresenceFlowCompletedArgs args)
        {
            // When the underlying MQTTv5 connection client has re-established its presence, then it is ready to use (analogous to an MQTTv3 client that has finished re-subscribing to twin/direct methods/telemetry topics)
            await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
        }

        internal override MqttConnect IotHubMqttConnectOverride(MqttConnect connect)
        {
            Debug.Assert(CurrentConnectionContext != null);

            connect.WebsocketUri = $"wss://{connect.HostName}/$iothub/websocket";
            connect.ProtocolVersion = CurrentConnectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5 ? MqttProtocolVersion.V500 : MqttProtocolVersion.V311;
            connect.Username = $"{connect.HostName}/{connect.ClientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";

            // MQTTv3 flow does not need to insert anything unique per connect attempt
            return connect;
        }

        public override async Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            // Note that this callback handler is only set after provisioning has completed. It is only to respond to IoT Hub connection events, not DPS connection events.

            // This callback should only be reached after provisioning, so their should always be a connection context to use
            Debug.Assert(CurrentConnectionContext != null);

            // Upon an MQTT connection being established, immediately re-subscribe to all twin/telemetry/direct methods topics if there is no session present.
            if (args.ConnectAck.IsSessionPresent)
            {
                await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });

                return;
            }

            MqttSubscribe mqttSubscribe = new();
            var expectedQos = MqttQualityOfServiceLevel.AtMostOnce;
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
                await ManagedMqttConnection.DisconnectAsync(true, new MqttDisconnect() { Reason = MqttDisconnectReasonCode.NormalDisconnection, SessionExpiryInterval = 0 });

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
        /// <exception cref="NotSupportedException">The client is not connected to IoT hub.</exception>
        /// <exception cref="InvalidOperationException">
        /// The connection's <see cref="Authentication.AuthenticationProvider.HasCertificateSigningHandler"/> is false,
        /// so it has no private key or callback to turn the issued certificates into an <see cref="X509Certificate2"/>.
        /// </exception>
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(IotHubCertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            // The connection context survives a disconnect, so the endpoint is what says whether this client is connected.
            if (CurrentConnectionContext == null || CurrentEndpoint != ConnectionEndpoint.IotHub)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            if (!CurrentConnectionContext.AuthenticationProvider.HasCertificateSigningHandler)
            {
                throw new InvalidOperationException(
                    $"Sending a certificate signing request requires the connection's authentication provider to be " +
                    "configured with the certificate signing request's private key or a callback that converts issued certificates into an X509Certificate2.");
            }

            CertificateSigningOperation operation = new();

            _pendingCertificateSigningOperations.TryAdd(request.RequestId, operation);

            await ManagedMqttConnection.SubscribeAsync(new(CertificateSigningResponseTopicFilter, MqttQualityOfServiceLevel.AtLeastOnce)); // TODO QoS correct?

            MqttPublish certificateSigningRequestPublish = new()
            {
                Topic = CertificateSigningRequestTopic + request.RequestId,
                Payload = JsonSerializer.SerializeToUtf8Bytes(request),
            };

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            MqttPublishAck puback = await ManagedMqttConnection.PublishAsync(certificateSigningRequestPublish, cancellationToken: cancellationToken);

            return operation;
        }

        private async Task HandleReceivedCertificateSigningPublish(MqttPublishReceivedEventArgs args)
        {
            if (args.Publish.Topic.StartsWith(CertificateSigningResponseTopic))
            {
                try
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
                        return;
                    }
                    else if (status.Equals("200"))
                    {
                        CertificateSigningResponse response = JsonSerializer.Deserialize<CertificateSigningResponse>(args.Publish.Payload)!;
                        // The authentication provider receives these via GetIotHubCredentialsAsync on the next connection attempt to IoT hub.
                        //TODO need a fault-injection like unit test that ensures that the client presents these new certificates upon reconnect since our API won't allow users to disconnect then reconnect to hub at will
                        if (CurrentConnectionContext != null)
                        {
                            CurrentConnectionContext.IssuedClientCertificates = response.Certificates;

                            if (response.Certificates is { Count: > 0 })
                            {
                                // Resolve the credentials now so that a provider that cannot use the issued certificates
                                // fails this operation rather than the next connection attempt.
                                try
                                {
                                    await CurrentConnectionContext.AuthenticationProvider.GetIotHubCredentialsAsync(
                                        CurrentConnectionContext.IotHubHostName,
                                        CurrentConnectionContext.DeviceId,
                                        response.Certificates);
                                }
                                catch (Exception ex)
                                {
                                    Trace.TraceError("Authentication provider failed to handle the client certificates issued by IoT hub. {0}", ex);
                                    pendingCertificateSigningOperation.SetFailed(ex);
                                    return;
                                }
                            }
                        }

                        pendingCertificateSigningOperation.SetCompleted(response);
                        return;
                    }
                    else
                    {
                        CertificateSigningRequestErrorResponse error = JsonSerializer.Deserialize<CertificateSigningRequestErrorResponse>(args.Publish.Payload)!;
                        pendingCertificateSigningOperation.SetFailed(new CertificateSigningRequestFailedException() { Error = error });
                        return;
                    }
                }
                finally
                {
                    await args.AcknowledgeAsync(CancellationToken.None);
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
            const string name = "Microsoft.Azure.Iot.Device";

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

        internal override bool DoesClientSupportHubType(ConnectionProfile connectionProfile)
        {
            return connectionProfile == ConnectionProfile.Classic;
        }
    }
}
