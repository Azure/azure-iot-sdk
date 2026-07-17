using Google.Protobuf;
using Google.Protobuf.WellKnownTypes;
using Microsoft.Azure.Devices.Client.DirectMethods;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Telemetry;
using Microsoft.Azure.Devices.Client.Twin;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.IotHub
{
    internal class IotHubConnection //TODO maybe just move this code into connection client?
    {
        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        private const bool UseSubscribeElide = false; // Maybe user-configurable? It is a very small optimization that is probably more risk than it is worth for .NET users compared to C users
        private static TimeSpan birthAckReceivedDefensiveTimeout = TimeSpan.FromSeconds(5); //TODO value is magic number
        private static TimeSpan twinPushReceivedDefensiveTimeout = TimeSpan.FromSeconds(5); //TODO value is magic number

        internal async Task<Twin.Twin> ConnectToAzureEventGridIotHubAsync(IMqttClient mqttClient, string hostname, string deviceId, X509AuthenticationProvider x509AuthenticationProvider, TwinPushOptions? twinPushOptions, CancellationToken cancellationToken = default)
        {
            bool subscribed = false;

            while (true) // Loop until presence is established successfully (return called) or user signals cancellation
            {
                cancellationToken.ThrowIfCancellationRequested();

                Trace.TraceInformation("Attempting to establish connection and presence for device {0} with IoT Hub {1}", deviceId, hostname);

                //TODO verify this is 16 bytes
                Guid connectNonce = Guid.NewGuid(); //Note that this nonce must be unique per connection attempt, not per successfuly connection

                string clientId = deviceId;

                string hexEncodedConnectNonce = BitConverter.ToString(Encoding.UTF8.GetBytes(connectNonce.ToString())).Replace("-", "");

                // Should look something like "correlationId=4f3c2a1b9d8e47f0a1b2c3d4e5f60718&clientVersion=csharp%2F1.42.0"
                // TODO do we want to also include previous user agent details like OS, architecture, etc? Service currently discards those
                string username = $"correlationId={Uri.EscapeDataString(hexEncodedConnectNonce)}&clientVersion={Uri.EscapeDataString($"csharp/{GetPackageVersion()}")}";

                MqttConnect connectPacket = new MqttConnect()
                {
                    HostName = hostname,
                    TcpPort = 8883,
                    WebsocketPort = 443,
                    WebsocketUri = $"wss://{hostname}/$iothub/websocket",
                    ClientCertificate = x509AuthenticationProvider.ClientCertificate,
                    CleanSession = true, // TODO user configurable?
                    Username = hexEncodedConnectNonce,
                    Password = Array.Empty<byte>(),
                    ClientId = clientId,
                    ProtocolVersion = MqttProtocolVersion.V311
                };

                MqttConnectAck connack;
                try
                {
                    connack = await mqttClient.ConnectAsync(connectPacket, cancellationToken);

                }
                catch (Exception ex)
                {
                    Trace.TraceWarning("Exception thrown while connecting to MQTT broker: {0}. Attempting connection again...", ex.Message);
                    continue; // Start the connect process over again
                }

                if (connack.ResultCode != MqttConnectResultCode.Success)
                {
                    subscribed = false;
                    Trace.TraceWarning("Received CONNACK with unsuccessful result code: {0}. Attempting connection again...", connack.ResultCode);
                    continue; // Start the connect process over again
                }

                // Only send the subscribe if it hasn't been sent in this MQTT session yet and 
                if (!(connack.IsSessionPresent && subscribed) && UseSubscribeElide)
                {
                    subscribed = false;

                    try
                    {
                        // TODO this feels a bit optimistic since there is a chance that the session was established -> connection lost happened on the previous connection prior to this subscribe happening
                        var suback = await mqttClient.SubscribeAsync(new(string.Format("ih/{deviceId}/dev/#", deviceId), MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken);
                        if (suback.Items.FirstOrDefault().ResultCode != MqttClientSubscribeResultCode.GrantedQoS1)
                        {
                            Trace.TraceWarning("Received SUBACK on devicebound SUBSCRIBE with unsuccessful result code: {0}. Attempting connection again...", suback.Items.FirstOrDefault().ResultCode);
                            continue; // Start the connect process over again
                        }

                    }
                    catch (Exception e)
                    {
                        Trace.TraceWarning("Exception thrown while subscribing to devicebound topic: {0}. Attempting connection again...", e.Message);
                        continue; // Start the connect process over again
                    }
                }

                subscribed = true;

                // The device's last known version of the twin's reported and desired properties. Currently, there is no support for persisting this state on disk, so assume the device has never seen the twin.
                uint deviceDesiredPropertyVersion = 0;
                uint deviceReportedPropertyVersion = 0;

                // TODO user configurable
                bool pushDesired = true;
                bool pushReported = true;

                Birth birth = new()
                {
                    SessionPresent = connack.IsSessionPresent,
                    ReportedVersion = deviceReportedPropertyVersion, // Service allows for device to "resume" its previously known state if device boots up and has twin in durable storage somewhere. Not something we supported in v1, though AFAIK
                    DesiredVersion = deviceDesiredPropertyVersion,

                    PushDesired = pushDesired, // If false, users will only get desired properties via a GetTwin call. Akin to subscribing to desired properties in v1 land
                    PushReported = pushReported, // If true, all current reported properties will be pushed to this device to "re-hydrate" its reported properties state. Not analogous to anything in v1 AFAIK
                };

                MqttPublish birthMessage = new MqttPublish()
                {
                    Topic = string.Format("ih/{deviceId}/srv/presence", deviceId),
                    CorrelationData = connectNonce.ToByteArray(),
                    Payload = birth.ToByteArray(),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce, // QoS 0 because we don't care about the MQTT-level ack for this message.  The service will send a fully-fledged MQTT publish as the ack and we will listen for that below
                };

                birthMessage.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth:1")));

                TaskCompletionSource<BirthAck> birthAckReceivedTaskCompletionSource = new();
                Func<MqttPublishReceivedEventArgs, Task> HandleReceivedBirthAck = (args) =>
                {
                    MqttPublish publish = args.Publish;
                    if (publish.Topic.Equals(string.Format("ih/{deviceId}/dev/presence", deviceId)))
                    {
                        if (publish.UserProperties.TryGetType(out string? messageType, out int? version))
                        {
                            if (messageType.Equals("birth-ack")
                                && GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedGuid)
                                && receivedGuid.Equals(connectNonce))
                            {
                                // The birth message flow is only complete once Hub sends a birth message ack with connection epoch equal to the latest connection epoch we have attempted
                                birthAckReceivedTaskCompletionSource.TrySetResult(BirthAck.Parser.ParseFrom(args.Publish.Payload));
                            }
                        }
                    }

                    return Task.CompletedTask;
                };

                TaskCompletionSource<TwinPush> twinPushReceivedTaskCompletionSource = new();
                Func<MqttPublishReceivedEventArgs, Task> HandleReceivedTwinPush = (args) =>
                {
                    MqttPublish publish = args.Publish;
                    if (publish.Topic.Equals(string.Format("ih/{deviceId}/dev/presence", deviceId)))
                    {
                        if (publish.UserProperties.TryGetType(out string? messageType, out int? version))
                        {
                            if (messageType.Equals("twin-push")
                                && GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedGuid)
                                && receivedGuid.Equals(connectNonce))
                            {
                                twinPushReceivedTaskCompletionSource.TrySetResult(TwinPush.Parser.ParseFrom(args.Publish.Payload));
                            }
                        }
                    }

                    return Task.CompletedTask;
                };

                mqttClient.PublishReceivedAsync += HandleReceivedBirthAck;
                mqttClient.PublishReceivedAsync += HandleReceivedTwinPush;

                MqttPublishAck birthMessagePuback;
                try
                {
                    birthMessagePuback = await mqttClient.PublishAsync(birthMessage, cancellationToken);
                }
                catch (Exception e)
                {
                    Trace.TraceWarning("Exception thrown while publishing birth message: {0}. Disconnecting from the MQTT broker and attempting connection again...", e.Message);
                    
                    await mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "MQTT client threw an exception while sending birth message" }, cancellationToken);
                    subscribed = false;

                    mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
                    mqttClient.PublishReceivedAsync -= HandleReceivedTwinPush;
                    continue; // Start the connect process over again
                }

                if (birthMessagePuback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceWarning("Received unsuccessful PUBACK when publishing birth message: {0}. Disconnecting from the MQTT broker and attempting connection again...", birthMessagePuback);
                    
                    await mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "Birth message wasn't sent successfully" }, cancellationToken);
                    subscribed = false;

                    mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
                    mqttClient.PublishReceivedAsync -= HandleReceivedTwinPush;
                    continue; // // Start the whole connect process over again
                }

                BirthAck birthAck;
                try
                {
                    birthAck = await birthAckReceivedTaskCompletionSource.Task.WaitAsync(birthAckReceivedDefensiveTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    // Did not receive mqtt birth ack message in timely manner (and user has not canceled this function yet)
                    Trace.TraceWarning("Timed out waiting for birth ack message. Disconnecting from the MQTT broker and attempting connection again...");
                    
                    await mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "Timed out waiting for birth-ack publish" }, cancellationToken);
                    subscribed = false;

                    // MQTT keep-alive is what guarantees the device-broker connection is alive. As long as keep-alive is healthy, the device is connected to the broker, and the nominal expectation is that birth-ack arrives, full stop. A birth-ack timeout firing on a connection that keep-alive still considers healthy is therefore an unambiguous signal of system degeneration somewhere the device cannot influence: a slow backend, EG/EH egress lag, an in-broker dispatch stall. The SDK's only sensible response is to wait long enough for the degenerate component to recover before adding more load to it.
                    /*
                     Attempt 1: base 5 min + uniform jitter [0, 5 min] → range 5–10 min.
                     Attempt 2: base 6 min + uniform jitter [0, 5 min] → range 6–11 min.
                     Attempt 3: base 8 min + uniform jitter [0, 5 min] → range 8–13 min.
                     Attempt 4 and beyond: base 10 min + uniform jitter [0, 5 min] → range 10–15 min. 
                     */
                    //TODO add delays here before next connect attempt according to service spec above

                    mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
                    mqttClient.PublishReceivedAsync -= HandleReceivedTwinPush;
                    continue; // Start the whole connect process over again
                }

                // Birth ack was received, so stop listening for birth acks.
                mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;


                // The authoritative versions as understood by IoT hub
                var authoritativeReportedVersion = birthAck.ReportedVersion;
                var authoritativeDesiredVersion = birthAck.DesiredVersion;

                // By default, the device will not fetch the current twin as part of this connect flow. Setting either of the pushReported or pushDesired flags allows the service to re-hydrate the device's understanding of twin state.
                Twin.Twin currentTwin = new();

                // If the device's twin is out of date in any way, and the user wants to re-hydrate reported or desired properties, then wait for the service to send the "twin push" message with that state
                if ((authoritativeReportedVersion > deviceReportedPropertyVersion || authoritativeDesiredVersion > deviceDesiredPropertyVersion)
                    && (pushReported || pushDesired))
                {
                    TwinPush receivedTwinPush;
                    try
                    {
                        receivedTwinPush = await twinPushReceivedTaskCompletionSource.Task.WaitAsync(twinPushReceivedDefensiveTimeout, cancellationToken);
                    }
                    catch (TimeoutException)
                    {
                        Trace.TraceWarning("Timed out waiting for an expected twin push message. Disconnecting from the MQTT broker and attempting connection again...");

                        await mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "Timed out waiting for twin push publish" }, cancellationToken);
                        subscribed = false;

                        mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
                        mqttClient.PublishReceivedAsync -= HandleReceivedTwinPush;
                        continue; // Start the whole connect process over again
                    }

                    if (receivedTwinPush.Desired != null)
                    {
                        currentTwin.Desired = JsonNode.Parse(receivedTwinPush.Desired.Payload.Span)!.AsObject();
                        currentTwin.ReportedVersion = receivedTwinPush.Desired.Version;
                    }

                    if (receivedTwinPush.Reported != null)
                    {
                        currentTwin.Reported = JsonNode.Parse(receivedTwinPush.Reported.Payload.Span)!.AsObject();
                        currentTwin.ReportedVersion = receivedTwinPush.Reported.Version;
                    }
                }

                mqttClient.PublishReceivedAsync -= HandleReceivedTwinPush;

                // Device presence was established and the initial twin push was received (if one was requested), so device connection has completed
                return currentTwin;
            }
        }

        internal async Task<Twin.Twin> ConnectToClassicIotHubAsync(IMqttClient mqttClient, string hostname, string deviceId, X509AuthenticationProvider x509AuthenticationProvider, ConnectionClient connectionClient, TwinPushOptions? twinPushOptions = null, CancellationToken cancellationToken = default)
        {
            string clientId = deviceId;
            //TODO what is the latest Hub API version?
            string username = $"{hostname}/{clientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";

            twinPushOptions ??= new();

            MqttConnect connectPacket = new MqttConnect()
            {
                HostName = hostname,
                TcpPort = 8883,
                WebsocketPort = 443,
                WebsocketUri = $"wss://{hostname}/$iothub/websocket",
                ClientCertificate = x509AuthenticationProvider.ClientCertificate,
                CleanSession = true, //TODO user configurable value
                Username = username,
                Password = Array.Empty<byte>(),
                ClientId = clientId,
                ProtocolVersion = MqttProtocolVersion.V311
            };

            var connack = await mqttClient.ConnectAsync(connectPacket, cancellationToken);

            ConnectRejectedException.ThrowIfUnsuccessfulConnack(connack, "Connection to IoT Hub was rejected.");

            //TODO feels a bit weird to do these subs outside of the method client/twin client, and it forces the user to construct their direct method/twin clients
            //and set their callbacks before connecting, but not sure what other approach works when AEG style hub mandates subscriptions as part of the connect birth message

            //TODO check for previous connack isSessionPresent flag before firing off all these subscriptions?
            MqttSubscribe mqttSubscribe = new();
            var expectedQos = MqttQualityOfServiceLevel.AtMostOnce;
            //mqttSubscribe.TopicFilters.Add(new(string.Format(TelemetryClient.DeviceBoundMessagesTopicFormat + "#", deviceId), expectedQos)); // C2D not currently supported
            mqttSubscribe.TopicFilters.Add(new(TwinClient.ClassicTwinResponseTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(DirectMethodClient.ClassicDirectMethodsRequestTopic + "#", expectedQos));
            var suback = await mqttClient.SubscribeAsync(mqttSubscribe, cancellationToken);

            foreach (var topicSuback in suback.Items)
            {
                if (topicSuback.ResultCode != MqttClientSubscribeResultCode.GrantedQoS0)
                {
                    throw new Exception("TODO");
                }
            }

            Twin.Twin twinPush = new();

            // This feature was introduced in AEG, and this block attempts to mimic that same behavior to the user. It does not have the same
            // ability to actually specify to the service that the client wants just the reported properties or just the desired properties. It
            // also lacks the ability to prevent the push depending on if-match flags. But this is as close as it gets to matching AEG-specific behavior?
            if (twinPushOptions.ReceiveReportedPropertiesUponConnect || twinPushOptions.ReceiveDesiredPropertyUpdates)
            {
                //TODO ewwwwwww
                TwinClient twinClient = new(connectionClient);
                var currentTwin = await twinClient.GetTwinAsync(twinPushOptions.ReceiveReportedPropertiesUponConnect, twinPushOptions.ReceiveDesiredPropertyUpdates, 0, 0, cancellationToken);

                if (twinPushOptions.ReceiveDesiredPropertyUpdates)
                {
                    twinPush.Desired = currentTwin.Desired;
                    twinPush.DesiredVersion = currentTwin.DesiredVersion;
                }

                if (twinPushOptions.ReceiveReportedPropertiesUponConnect)
                {
                    twinPush.Reported = currentTwin.Reported;
                    twinPush.ReportedVersion = currentTwin.ReportedVersion;
                }
            }

            return twinPush;
        }

        private string GetUserAgentString()
        {
            const string name = "Microsoft.Azure.Devices.Client";

            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            string userAgent = $"{name}/{GetPackageVersion()} ({runtime}; {operatingSystem}; {processorArchitecture})";

            return userAgent;
        }

        private string GetPackageVersion()
        {
            return typeof(IotHubConnection).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }
    }
}
