using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models.Twin;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    internal class IotHubConnection //TODO maybe just move this code into connection client?
    {
        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        private const bool UseSubscribeElide = false; // Maybe user-configurable? It is a very small optimization that is probably more risk than it is worth for .NET users compared to C users
        private static TimeSpan birthAckReceivedDefensiveTimeout = TimeSpan.FromSeconds(60); //TODO value is magic number
        private static TimeSpan twinPushReceivedDefensiveTimeout = TimeSpan.FromSeconds(60); //TODO value is magic number

        internal static async Task ConnectToAzureEventGridIotHubAsync(IMqttClient mqttClient, string hostname, string deviceId, X509AuthenticationProvider x509AuthenticationProvider, TwinPushOptions? twinPushOptions, CancellationToken cancellationToken = default)
        {
            bool subscribed = false;

            while (true) // Loop until presence is established successfully (return called) or user signals cancellation
            {
                cancellationToken.ThrowIfCancellationRequested();

                Trace.TraceInformation("Attempting to establish connection and presence for device {0} with IoT Hub {1}", deviceId, hostname);

                Guid connectNonce = Guid.NewGuid(); //Note that this nonce must be unique per connection attempt, not per successfuly connection

                string clientId = deviceId;

                string hexEncodedConnectNonce = Convert.ToHexString(connectNonce.ToByteArray(bigEndian: true));

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
                    Username = username,
                    Password = Array.Empty<byte>(),
                    ClientId = clientId,
                    ProtocolVersion = MqttProtocolVersion.V500
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

                // Only send the subscribe if it hasn't been sent in this MQTT session yet
                if (!subscribed || !connack.IsSessionPresent) //TODO subscribe elide logic
                {
                    subscribed = false;

                    try
                    {
                        // TODO this feels a bit optimistic since there is a chance that the session was established -> connection lost happened on the previous connection prior to this subscribe happening
                        var suback = await mqttClient.SubscribeAsync(new(string.Format("ih/{0}/dev/#", deviceId), MqttQualityOfServiceLevel.AtLeastOnce), cancellationToken);
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
                    Topic = string.Format("ih/{0}/srv/presence", deviceId),
                    CorrelationData = connectNonce.ToByteArray(bigEndian: true),
                    Payload = birth.ToByteArray(),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce, // QoS 0 because we don't care about the MQTT-level ack for this message.  The service will send a fully-fledged MQTT publish as the ack and we will listen for that below
                };

                birthMessage.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth:1")));

                TaskCompletionSource<BirthAck> birthAckReceivedTaskCompletionSource = new();
                Func<MqttPublishReceivedEventArgs, Task> HandleReceivedBirthAck = async (args) =>
                {
                    // Birth ack messages are QoS 0, so no need to ack
                    MqttPublish publish = args.Publish;
                    if (publish.Topic.Equals(string.Format("ih/{0}/dev/presence", deviceId)))
                    {
                        if (publish.UserProperties.TryGetType(out string? messageType, out int? version))
                        {
                            if (messageType.Equals("birth-ack"))
                            {
                                if (GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? receivedGuid))
                                {
                                    if (receivedGuid.Equals(connectNonce))
                                    {
                                        // The birth message flow is only complete once Hub sends a birth message ack with connection epoch equal to the latest connection epoch we have attempted
                                        birthAckReceivedTaskCompletionSource.TrySetResult(BirthAck.Parser.ParseFrom(args.Publish.Payload));
                                    }
                                    else
                                    {
                                        Trace.TraceWarning("Received birth ack, but for an unexpected connection nonce. Expected {0}, but was {1}", connectNonce.ToString(), receivedGuid.ToString());
                                    }
                                }
                                else
                                { 
                                    Trace.TraceWarning("Received birth ack, with a malformed connection nonce");
                                }
                            }
                        }
                    }
                };

                TaskCompletionSource<TwinPush> twinPushReceivedTaskCompletionSource = new();

                mqttClient.PublishReceivedAsync += HandleReceivedBirthAck;

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
                    continue; // Start the connect process over again
                }

                if (birthMessagePuback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceWarning("Received unsuccessful PUBACK when publishing birth message: {0}. Disconnecting from the MQTT broker and attempting connection again...", birthMessagePuback);
                    
                    await mqttClient.DisconnectAsync(new MqttDisconnect() { Reason = MqttClientDisconnectOptionsReason.NormalDisconnection, ReasonString = "Birth message wasn't sent successfully" }, cancellationToken);
                    subscribed = false;

                    mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
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
                    continue; // Start the whole connect process over again
                }

                // Birth ack was received, so stop listening for birth acks.
                mqttClient.PublishReceivedAsync -= HandleReceivedBirthAck;
            }
        }

        private static string GetPackageVersion()
        {
            return typeof(IotHubConnection).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }
    }
}
