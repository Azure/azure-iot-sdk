// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Provisioning.Models;
using Microsoft.Azure.Devices.Client.Serialization;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Provisioning
{
    internal sealed class ProvisioningConnection
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

        internal async Task<DeviceRegistrationResult> RegisterAsync(
            IMqttClient mqttClient,
            RegistrationRequestPayload payload,
            X509AuthenticationProvider authentication,
            string idScope,
            string globalDeviceEndpoint,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();

            MqttConnect connect = CreateMqttConnectPacket(authentication, idScope, globalDeviceEndpoint);
            mqttClient.PublishReceivedAsync += HandleReceivedPublishAsync;

            using var connectionLostCancellationToken = new CancellationTokenSource();

            // Link the user-supplied cancellation token with a cancellation token that is cancelled
            // when the connection is lost so that all operations stop when either the user
            // cancels the token or when the connection is lost.
            using var linkedCancellationToken = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                connectionLostCancellationToken.Token);

            Task HandleDisconnectionAsync(MqttClientDisconnectedEventArgs disconnectedEventArgs)
            {
                // If it was an unexpected disconnect. Ignore cases when the user intentionally closes the connection.
                connectionLostCancellationToken.Cancel();
                return Task.CompletedTask;
            }

            // Additional context to be included in the error message thrown if the connection is lost to explain
            // when the connection was lost. Mostly for e2e test debugging, but users may find this helpful as well.
            MqttConnectAck connectResult = await mqttClient
                .ConnectAsync(connect, cancellationToken)
                .ConfigureAwait(false);

            if (connectResult.ResultCode != MqttConnectResultCode.Success)
            {
                throw new Exception("TODO: " + connectResult.ResultCode);
            }

            mqttClient.DisconnectedAsync += HandleDisconnectionAsync;

            await SubscribeToRegistrationResponseMessagesAsync(mqttClient, linkedCancellationToken.Token).ConfigureAwait(false);

            RegistrationOperationStatus registrationStatus = await PublishRegistrationRequestAsync(
                    mqttClient,
                    payload,
                    linkedCancellationToken.Token)
                .ConfigureAwait(false);

            DeviceRegistrationResult registrationResult = await PollUntilProvisionigFinishesAsync(
                    mqttClient,
                    registrationStatus.OperationId,
                    linkedCancellationToken.Token)
                .ConfigureAwait(false);

            mqttClient.PublishReceivedAsync -= HandleReceivedPublishAsync;
            mqttClient.DisconnectedAsync -= HandleDisconnectionAsync;
            var disconnect = new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection };

            try
            {
                await mqttClient.DisconnectAsync(disconnect, cancellationToken);
            }
            catch (Exception)
            {
                // Deliberately not rethrowing the exception because this is a "best effort" close.
                // The service may not have acknowledged that the client closed the connection, but
                // all local resources have been closed. The service will eventually realize the
                // connection is closed in cases like these.
            }

            return registrationResult;

        }

        private async Task SubscribeToRegistrationResponseMessagesAsync(IMqttClient mqttClient, CancellationToken cancellationToken)
        {
            try
            {
                MqttSubscribeAck subscribeResults = await mqttClient.SubscribeAsync(new(SubscribeFilter, MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken).ConfigureAwait(false);

                if (subscribeResults.Items.FirstOrDefault()!.ResultCode != MqttClientSubscribeResultCode.GrantedQoS1)
                {
                    throw new Exception("todo");
                }
            }
            catch (Exception ex)
            {
                throw new Exception("todo: " + ex.Message);
            }
        }

        private async Task<RegistrationOperationStatus> PublishRegistrationRequestAsync(
            IMqttClient mqttClient,
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

            try
            {
                MqttPublishAck puback = await mqttClient.PublishAsync(publish, cancellationToken).ConfigureAwait(false);

                if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    //TODO
                }
            }
            catch (Exception)
            {
                //TODO
            }

            RegistrationOperationStatus registrationStatus = await _startProvisioningRequestStatusSource.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

            return registrationStatus.Status != ProvisioningRegistrationStatus.Assigning
                ? throw new Exception("TODO")
                : registrationStatus;
        }

        private async Task<DeviceRegistrationResult> PollUntilProvisionigFinishesAsync(IMqttClient mqttClient, string operationId, CancellationToken cancellationToken)
        {
            while (true)
            {
                string topicName = string.Format(CultureInfo.InvariantCulture, GetOperationsTopic, ++_requestId, operationId);
                MqttPublish message = new MqttPublish()
                {
                    Topic = topicName,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
                };

                _checkRegistrationOperationStatusSource = new TaskCompletionSource<RegistrationOperationStatus>(TaskCreationOptions.RunContinuationsAsynchronously);

                MqttPublishAck puback = await mqttClient.PublishAsync(message, cancellationToken).ConfigureAwait(false);

                if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    throw new Exception("TODO");
                }

                RegistrationOperationStatus currentStatus = await _checkRegistrationOperationStatusSource.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

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
                // TODO feels a little clunky to pass down both the TCP port + TCP host and the WS uri + WS port
                HostName = hostName,
                TcpPort = 8883,
                WebsocketPort = 443,
                WebsocketUri = $"wss://{hostName}",
                ClientCertificate = authentication.ClientCertificate,
                CleanSession = true,
                Username = username,
                Password = Array.Empty<byte>(),
                ClientId = authentication.GetRegistrationId(),
                ProtocolVersion = MqttProtocolVersion.V311
            };
        }

        private Task HandleReceivedPublishAsync(MqttPublishReceivedEventArgs receivedEventArgs)
        {
            string topic = receivedEventArgs.Publish.Topic;

            if (_startProvisioningRequestStatusSource == null)
            {
                // TODO This seems to happen around reconnect scenarios? Not sure how though since we always connecto with clean session
                return Task.CompletedTask;
            }

            if (!_startProvisioningRequestStatusSource.Task.IsCompleted)
            {
                // The initial provisioning request's response topic is shaped like "$dps/registrations/res/202/?$rid=1&retry-after=3"
                string jsonString = Encoding.UTF8.GetString(receivedEventArgs.Publish.Payload);
                RegistrationOperationStatus operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(jsonString, JsonSerializationSettings.Options)!;
                _startProvisioningRequestStatusSource.TrySetResult(operation);
            }
            else
            {
                // All status polling requests' response topics are shaped like "$dps/registrations/res/200/?$rid=2"
                string jsonString = Encoding.UTF8.GetString(receivedEventArgs.Publish.Payload);
                try
                {
                    RegistrationOperationStatus operation = JsonSerializer.Deserialize<RegistrationOperationStatus>(jsonString, JsonSerializationSettings.Options)!;
                    operation.RetryAfter = GetRetryAfterFromTopic(topic, s_defaultOperationPollingInterval);

                    _checkRegistrationOperationStatusSource!.TrySetResult(operation);
                }
                catch (Exception e)
                {
                    throw e;
                }
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
