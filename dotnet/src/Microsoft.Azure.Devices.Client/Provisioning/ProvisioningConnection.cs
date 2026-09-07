// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Provisioning
{
    internal sealed class ProvisioningConnection //TODO disposable?
    {
        private const string UsernameFormat = "{0}/registrations/{1}/api-version={2}&ClientVersion={3}";
        private const string SubscribeFilter = "$dps/registrations/res/#";
        private const string RegisterTopic = "$dps/registrations/PUT/iotdps-register/?$rid={0}";
        private const string GetOperationsTopic = "$dps/registrations/GET/iotdps-get-operationstatus/?$rid={0}&operationId={1}";
        private const string RetryAfterHeader = "Retry-After";

        private static readonly TimeSpan s_defaultOperationPollingInterval = TimeSpan.FromSeconds(2);

        private TaskCompletionSource<RegistrationOperationStatus>? _startProvisioningRequestStatusSource;
        private TaskCompletionSource<RegistrationOperationStatus>? _checkRegistrationOperationStatusSource;
        private int _requestId;

        private MqttConnectionManager? _mqttClient;
        private RegistrationRequestPayload? _payload;

        /// <summary>
        /// Cancels everything the in-progress provisioning flow is waiting on. Because DPS cannot persist sessions,
        /// a flow is only valid for the connection it was started on, so this is cancelled whenever that connection ends.
        /// </summary>
        private CancellationTokenSource? _currentProvisioningFlowCancellation;

        /// <summary>
        /// Raised once the provisioning flow that runs upon connecting to DPS has either produced a registration
        /// result or failed. This is the provisioning analog of the device presence flow's completion event.
        /// </summary>
        private event Func<ProvisioningFlowCompletedArgs, Task>? ProvisioningFlowCompletedAsync;

        internal async Task<DeviceRegistrationResult> RegisterAsync(
            MqttConnectionManager mqttClient,
            RegistrationRequestPayload payload,
            X509AuthenticationProvider authentication,
            string idScope,
            string globalDeviceEndpoint,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();

            _mqttClient = mqttClient;
            _payload = payload;

            MqttConnect connect = CreateMqttConnectPacket(authentication, idScope, globalDeviceEndpoint);

            TaskCompletionSource<ProvisioningFlowCompletedArgs> provisioningFlowResult = new(TaskCreationOptions.RunContinuationsAsynchronously);
            Task HandleProvisioningFlowCompletedAsync(ProvisioningFlowCompletedArgs args)
            {
                provisioningFlowResult.TrySetResult(args);
                return Task.CompletedTask;
            }

            Task HandleConnectionFaultedAsync(MqttConnectionFaultedEventArgs faultedEventArgs)
            {
                // The connection layer has stopped maintaining the connection, so no further connection will arrive to
                // start the provisioning flow again.
                provisioningFlowResult.TrySetException(faultedEventArgs.Exception);
                return Task.CompletedTask;
            }

            // Setup callbacks BEFORE sending CONNECT so that the CONNACK can be handled regardless of how quickly it arrives
            ProvisioningFlowCompletedAsync += HandleProvisioningFlowCompletedAsync;
            mqttClient.PublishReceivedAsync += HandleReceivedPublishAsync;
            mqttClient.DisconnectedAsync += HandleDisconnectedFromDpsAsync;
            mqttClient.ConnectionFaultedAsync += HandleConnectionFaultedAsync;
            mqttClient.ConnectedAsync += HandleConnectedToDpsAsync;

            try
            {
                // MQTT connection manager already checks connack for non-success cases, so no need to check it here as well.
                // That layer also owns reconnection, and because DPS cannot persist sessions, every connection it establishes
                // starts the provisioning flow over from the beginning in HandleConnectedToDpsAsync.
                MqttConnectAck connack = await mqttClient.ConnectAsync(connect, cancellationToken).ConfigureAwait(false);

                ProvisioningFlowCompletedArgs provisioningFlowCompletedArgs = await provisioningFlowResult.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

                if (provisioningFlowCompletedArgs.Exception != null)
                {
                    throw provisioningFlowCompletedArgs.Exception;
                }

                Debug.Assert(provisioningFlowCompletedArgs.RegistrationResult != null);

                return provisioningFlowCompletedArgs.RegistrationResult;
            }
            finally
            {
                ProvisioningFlowCompletedAsync -= HandleProvisioningFlowCompletedAsync;
                mqttClient.ConnectedAsync -= HandleConnectedToDpsAsync;
                mqttClient.ConnectionFaultedAsync -= HandleConnectionFaultedAsync;
                mqttClient.DisconnectedAsync -= HandleDisconnectedFromDpsAsync;
                mqttClient.PublishReceivedAsync -= HandleReceivedPublishAsync;

                // Stop any provisioning flow that is still waiting on a DPS response now that no one is listening for its result.
                CancelCurrentProvisioningFlow();

                // Always close the MQTT connection once provisioning has finished so that the connection can be
                // re-established against the assigned IoT hub.
                var disconnect = new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection };

                try
                {
                    await mqttClient.DisconnectAsync(false, disconnect, CancellationToken.None).ConfigureAwait(false);
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
        /// Run the provisioning flow upon connecting to DPS, in the same way that the connection clients run the device
        /// presence flow upon connecting to IoT hub.
        /// </summary>
        /// <remarks>
        /// DPS cannot persist sessions, so the CONNACK's session present flag is deliberately ignored: every connection
        /// starts a brand new registration.
        /// </remarks>
        private async Task HandleConnectedToDpsAsync(MqttClientConnectedEventArgs args)
        {
            Debug.Assert(_mqttClient != null);
            MqttConnectionManager mqttClient = _mqttClient;

            // Any flow left over from a previous connection belongs to a session that no longer exists, so abandon it.
            CancelCurrentProvisioningFlow();

            var provisioningFlowCancellation = new CancellationTokenSource();
            _currentProvisioningFlowCancellation = provisioningFlowCancellation;

            // Responses to the previous connection's requests are no longer expected.
            _startProvisioningRequestStatusSource = null;
            _checkRegistrationOperationStatusSource = null;

            try
            {
                await SubscribeToRegistrationResponseMessagesAsync(mqttClient, provisioningFlowCancellation.Token).ConfigureAwait(false);

                RegistrationOperationStatus registrationStatus = await PublishRegistrationRequestAsync(
                        mqttClient,
                        _payload!,
                        provisioningFlowCancellation.Token)
                    .ConfigureAwait(false);

                DeviceRegistrationResult registrationResult = await PollUntilProvisioningFinishesAsync(
                        mqttClient,
                        registrationStatus.OperationId,
                        provisioningFlowCancellation.Token)
                    .ConfigureAwait(false);

                await RaiseProvisioningFlowCompletedAsync(new ProvisioningFlowCompletedArgs(registrationResult)).ConfigureAwait(false);
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
                await RaiseProvisioningFlowCompletedAsync(new ProvisioningFlowCompletedArgs(e)).ConfigureAwait(false);
            }
            finally
            {
                // Clear the field only if this flow is still the current one, so that a newer flow's cancellation source is left intact.
                Interlocked.CompareExchange(ref _currentProvisioningFlowCancellation, null, provisioningFlowCancellation);
                provisioningFlowCancellation.Dispose();
            }
        }

        private Task HandleDisconnectedFromDpsAsync(MqttClientDisconnectedEventArgs args)
        {
            // Because DPS cannot persist sessions, any connection loss should be treated as a session loss. Abandon the
            // in-progress flow so that it can be restarted from the beginning once the connection is re-established.
            CancelCurrentProvisioningFlow();

            return Task.CompletedTask;
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

        private async Task RaiseProvisioningFlowCompletedAsync(ProvisioningFlowCompletedArgs args)
        {
            Func<ProvisioningFlowCompletedArgs, Task>? handler = ProvisioningFlowCompletedAsync;

            if (handler != null)
            {
                await handler.Invoke(args).ConfigureAwait(false);
            }
        }

        private async Task SubscribeToRegistrationResponseMessagesAsync(MqttConnectionManager mqttClient, CancellationToken cancellationToken)
        {
            Trace.TraceInformation("Subscribing to DPS response topic {0}", SubscribeFilter);
            MqttSubscribeAck subscribeResults = await mqttClient.SubscribeAsync(new(SubscribeFilter, MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken).ConfigureAwait(false);

            if (subscribeResults.Items.FirstOrDefault()!.ReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1)
            {
                throw new Exception("todo");
            }
        }

        private async Task<RegistrationOperationStatus> PublishRegistrationRequestAsync(
            MqttConnectionManager mqttClient,
            RegistrationRequestPayload payload,
            CancellationToken cancellationToken)
        {
            byte[] serializedPayload = Array.Empty<byte>();
            if (payload != null)
            {
                string requestString = JsonSerializer.Serialize(payload, JsonSerializationSettings.Options);
                serializedPayload = Encoding.UTF8.GetBytes(requestString);
            }

            string registrationTopic = string.Format(CultureInfo.InvariantCulture, RegisterTopic, ++_requestId);
            MqttPublish publish = new MqttPublish()
            {
                Payload = serializedPayload,
                Topic = registrationTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
            };

            _startProvisioningRequestStatusSource = new TaskCompletionSource<RegistrationOperationStatus>(TaskCreationOptions.RunContinuationsAsynchronously);

            Trace.TraceInformation("Publishing to DPS on topic {0}", registrationTopic);

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            MqttPublishAck puback = await mqttClient.PublishAsync(publish, cancellationToken).ConfigureAwait(false);

            Trace.TraceInformation("Successfully published registration request to DPS with request Id {0}", _requestId);

            try
            {
                RegistrationOperationStatus registrationStatus = await _startProvisioningRequestStatusSource.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

                return registrationStatus.Status != ProvisioningRegistrationStatus.Assigning
                    ? throw new Exception("TODO")
                    : registrationStatus;
            }
            catch (OperationCanceledException e)
            {
                throw new OperationCanceledException("Timed out waiting for DPS to send the initial provisioning response", e);
            }
        }

        private async Task<DeviceRegistrationResult> PollUntilProvisioningFinishesAsync(MqttConnectionManager mqttClient, string operationId, CancellationToken cancellationToken)
        {
            while (true)
            {
                string topic = string.Format(CultureInfo.InvariantCulture, GetOperationsTopic, ++_requestId, operationId);
                MqttPublish message = new MqttPublish()
                {
                    Topic = topic,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
                };

                _checkRegistrationOperationStatusSource = new TaskCompletionSource<RegistrationOperationStatus>(TaskCreationOptions.RunContinuationsAsynchronously);

                Trace.TraceInformation("Publishing to DPS on topic {0}", topic);

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await mqttClient.PublishAsync(message, cancellationToken).ConfigureAwait(false);

                RegistrationOperationStatus currentStatus;
                try
                {
                    currentStatus = await _checkRegistrationOperationStatusSource.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
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

                await Task.Delay(pollingDelay, cancellationToken).ConfigureAwait(false);
            }
        }

        private MqttConnect CreateMqttConnectPacket(X509AuthenticationProvider authentication, string idScope, string globalDeviceEndpoint)
        {
            string hostName = globalDeviceEndpoint;
            
            string username = string.Format(
                CultureInfo.InvariantCulture,
                UsernameFormat,
                idScope,
                authentication.GetRegistrationId(),
                "2019-03-31",
                Uri.EscapeDataString(GetUserAgentString()));


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

        private Task HandleReceivedPublishAsync(MqttPublishReceivedEventArgs receivedEventArgs)
        {
            string topic = receivedEventArgs.Publish.Topic;

            TaskCompletionSource<RegistrationOperationStatus>? startProvisioningRequestStatusSource = _startProvisioningRequestStatusSource;

            if (startProvisioningRequestStatusSource == null)
            {
                // No registration request is outstanding on the current connection, so this publish belongs to a
                // provisioning flow that was abandoned when its connection ended.
                return Task.CompletedTask;
            }

            Trace.TraceInformation("Received MQTT publish from DPS on topic {0}", receivedEventArgs.Publish.Topic);

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

        private string GetUserAgentString()
        {
            const string name = "Microsoft.Azure.Devices.Provisioning.Client";

            string version = typeof(ProvisioningConnection).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            string userAgent = $"{name}/{version} ({runtime}; {operatingSystem}; {processorArchitecture})";

            return userAgent;
        }
    }
}
