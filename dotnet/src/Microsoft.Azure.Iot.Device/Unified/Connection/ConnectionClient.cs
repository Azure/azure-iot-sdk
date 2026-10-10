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

        public override MqttConnect MqttConnectOverride(MqttConnect connect)
        {
            //Defer to gen 2 client
            Debug.Assert(CurrentConnectionContext != null);

            connect.WebsocketUri = $"wss://{connect.HostName}/$iothub/websocket";
            connect.ProtocolVersion = CurrentConnectionContext.ConnectionProfile == Provisioning.Models.ConnectionProfile.MqttV5 ? MqttProtocolVersion.V500 : MqttProtocolVersion.V311;
            connect.Username = $"{connect.HostName}/{connect.ClientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";
            connect.Password = Array.Empty<byte>();

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
        public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(IotHubCertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            //TODO how does hub respond if device loses connection at any point during this process?
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            if (CurrentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
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
                CertificateSigningOperation? pendingCertificateSigningOperation = null;
                string? requestId = null;
                bool hasRequestFinished = false;
                try
                {
                    string[] topicTokens = args.Publish.Topic.Split("/");
                    if (topicTokens.Length != 5)
                    {
                        return;
                    }

                    string status = topicTokens[3];
                    requestId = topicTokens[4].Split(RequestId)[1];

                    if (!_pendingCertificateSigningOperations.TryGetValue(requestId, out pendingCertificateSigningOperation))
                    {
                        return;
                    }

                    if (status.Equals("202"))
                    {
                        CertificateSigningRequestAccepted accepted = JsonSerializer.Deserialize<CertificateSigningRequestAccepted>(args.Publish.Payload)
                            ?? throw new JsonException("Certificate signing completion response was null.");
                        pendingCertificateSigningOperation.SetAccepted(accepted);
                        return;
                    }
                    else if (status.Equals("200"))
                    {
                        hasRequestFinished = true;
                        CertificateSigningResponse response = JsonSerializer.Deserialize<CertificateSigningResponse>(args.Publish.Payload)
                            ?? throw new JsonException("Certificate signing completion response was null.");
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
                        return;
                    }
                    else
                    {
                        hasRequestFinished = true;
                        CertificateSigningRequestErrorResponse error = JsonSerializer.Deserialize<CertificateSigningRequestErrorResponse>(args.Publish.Payload)
                            ?? throw new JsonException("Certificate signing completion response was null.");
                        pendingCertificateSigningOperation.SetFailed(new CertificateSigningRequestFailedException() { Error = error, RequestId = requestId });
                        return;
                    }
                }
                catch (JsonException ex)
                {
                    // An unreadable response must fail the operation rather than leave it pending forever
                    pendingCertificateSigningOperation?.SetFailed(new CertificateSigningRequestFailedException()
                    {
                        Error = new CertificateSigningRequestErrorResponse() { Message = "Failed to read the certificate signing response from IoT hub: " + ex.Message },
                        RequestId = requestId,
                    });
                }
                catch (Exception ex) when (pendingCertificateSigningOperation != null)
                {
                    // Includes the user's HandleCertificateSigningCompleteAsync callback throwing
                    pendingCertificateSigningOperation.SetFailed(ex);
                }
                finally
                {
                    // Any terminal outcome (success, hub error, unreadable response, or a throwing user callback) ends the operation, so stop tracking it locally
                    if (requestId != null && hasRequestFinished)
                    {
                        _pendingCertificateSigningOperations.TryRemove(requestId, out _);
                    }

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
