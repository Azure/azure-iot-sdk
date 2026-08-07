using Google.Protobuf;
using Microsoft.Azure.Devices.Client.DirectMethods.Models;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.DirectMethods
{
    /// <summary>
    /// A feature client for receiving and responding to direct method requests from IoT Hub.
    /// </summary>
    public class DirectMethodClient : IDisposable
    {
        private const string ProtobufContentType = "application/protobuf";

        // Only applicable for AEG Hub scenario. Maps from request Id (GUID) to ready Id (also GUID). When this client receives an Exec message, it should only notify the user about it
        // if this map contains the exec's request Id and it maps to the ready Id in the Exec message.
        private ConcurrentDictionary<Guid, ByteString> _pendingExpectedDirectMethodReadyIds = new();

        // Only applicable for AEG Hub scenario. Maps from request Id (GUID) to method name.
        private ConcurrentDictionary<Guid, string> _pendingExpectedDirectMethodNames = new();

        private IConnectionClient _connection;

        /// <summary>
        /// An event that executes whenever this device receives a direct method request from IoT hub. After executing the direct method, the device must
        /// provide a direct method response.
        /// </summary>
        public event Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? DirectMethodInvokedAsync;

        /// <summary>
        /// An event that executes whenever this device receives a direct method probe from IoT hub. This event is a precursor to receiving the direct method itself 
        /// and allows your application to decide whether it is ready to receive this direct method or not.
        /// </summary>
        /// <remarks>This feature is only supported by IoT hubs that use Azure Event Grid. Older IoT hubs will never send this probe.</remarks>
        public event Func<DirectMethodRequestProbeReceivedEventArgs, Task<ProbeAck>>? DirectMethodProbeReceivedAsync;

        /// <summary>
        /// Construct a new <see cref="DirectMethodClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <para>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided connection client must be connected prior
        /// to using this feature client to receive any direct methods.
        /// </remarks>
        /// <example>
        /// The recommended order to instantiate feature clients and the connection client is as follows:
        /// <code>
        /// // Construct all the clients your device will use
        /// ConnectionClient connectionClient = new();
        /// DirectMethodClient directMethodClient = new(connectionClient);
        /// 
        /// //Set all handlers 
        /// directMethodClient.DirectMethodProbeReceivedAsync += SomeProbeHandlingMethod;
        /// directMethodClient.DirectMethodInvokedAsync += SomeDirectMethodHandlingMethod;
        /// 
        /// // Open the connection (and start receiving direct methods)
        /// await connectionClient.ProvisionAndConnectAsync();
        /// </code>
        /// </example>
        public DirectMethodClient(IConnectionClient connection)
        {
            _connection = connection;
            _connection.MqttClient.PublishReceivedAsync+= HandleReceivedAzureEventGridMqttPublish;
        }

        internal DirectMethodClient(Unified.Connection.IConnectionClient connection)
        {
            _connection = new Gen2.Connection.ConnectionClient(connection);
            _connection.MqttClient.PublishReceivedAsync += HandleReceivedAzureEventGridMqttPublish;
        }

        private async Task HandleReceivedAzureEventGridMqttPublish(MqttPublishReceivedEventArgs args)
        {
            var currentConnectionContext = _connection.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                // This should never happen barring some race condition?
                Trace.TraceWarning("Received a direct method request, but the connection was lost. Ignoring it.");
                return;
            }

            if (!args.Publish.Topic.Equals(string.Format("ih/{0}/dev/methods", currentConnectionContext.DeviceId)))
            {
                // Message isn't relevant to this client. Ignore it.
                return;
            }

            if (DirectMethodInvokedAsync == null || DirectMethodProbeReceivedAsync == null)
            {
                Trace.TraceError("Received a direct method message, but this client has not registered both callbacks, so it cannot be handled.");
                return;
            }

            MqttPublish publish = args.Publish;
            if (!publish.UserProperties.TryGetType(out string? directMethodMessageType, out int? directMethodMessageTypeValue))
            {
                Trace.TraceWarning("Received a direct method message, but it is either missing the message type or the message type is malformed. Ignoring it.");
                return;
            }

            if (directMethodMessageTypeValue != 1)
            {
                // The service may increment the protocol version of these messages over time. For now, there is only the one version, though
                Trace.TraceWarning("Received a direct method message whose type version ({0}) is not supported by this client (supported version: {1}). You may need to upgrade this library's version to handle this kind of message. Ignoring it.", directMethodMessageTypeValue, 1);
                return;
            }

            // All AEG Hub direct method messages are QoS 1, so ack immediately upon recognizing the message as one
            await args.AcknowledgeAsync(CancellationToken.None);

            if (directMethodMessageType!.Equals("probe"))
            {
                if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
                {
                    return; // Malformed request. Discard it silently
                }

                uint connectTimeoutRemainingUponReceivingProbe = publish.MessageExpiryInterval;
                Stopwatch stopwatch = Stopwatch.StartNew();

                Probe probe = Probe.Parser.ParseFrom(publish.Payload);

                ProbeAck probeAck = await DirectMethodProbeReceivedAsync.Invoke(new() { MethodName = probe.MethodName, ResponseTimeoutSeconds = probe.ResponseTimeoutSeconds });

                stopwatch.Stop();

                uint secondsSinceReceivingProbe;
                try
                {
                    secondsSinceReceivingProbe = (uint)stopwatch.Elapsed.TotalSeconds;
                }
                catch (InvalidCastException)
                {
                    // Should only happen if the double grows so large that a uint can't contain it.
                    Trace.TraceError("Could not calculate time since probe message was received. This likely means the 'DirectMethodProbeReceivedAsync' callback took too long. Cannot send probe ack, so discarding this probe message");
                    return;
                }

                uint remainingConnectTimeoutInSeconds = connectTimeoutRemainingUponReceivingProbe - secondsSinceReceivingProbe;

                if (remainingConnectTimeoutInSeconds < 1)
                {
                    Trace.TraceWarning("Application did not respond to direct method probe message before the connect timeout elapsed, so no probe ack message will be sent.");
                    return;
                }

                MqttPublish probeAckPublish = new()
                {
                    Topic = string.Format("ih/{0}/srv/methods", currentConnectionContext.DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    Payload = probeAck.ToByteArray(),
                    CorrelationData = publish.CorrelationData,
                    MessageExpiryInterval = remainingConnectTimeoutInSeconds,
                    ContentType = ProtobufContentType
                };

                probeAckPublish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("probe-ack:1")));

                if (probeAck.Ready != default)
                {
                    // The user signalled that the device was ready for the direct method, so locally save the ready Id and request Id
                    _pendingExpectedDirectMethodReadyIds.TryAdd(requestId.Value, probeAck.Ready.ReadyId);
                    _pendingExpectedDirectMethodNames.TryAdd(requestId.Value, probe.MethodName);
                }

                MqttPublishAck puback = await _connection.MqttClient.PublishAsync(probeAckPublish);

                if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceError("Failed to send the response to a direct method probe because the MQTT broker rejected the publish with reason code {0} and reason string {1}", puback.ReasonCode, puback.ReasonString);
                }
            }
            else if (directMethodMessageType.Equals("exec"))
            {
                if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
                {
                    Trace.TraceWarning("Received direct method exec message with malformed request Id. Ignoring it.");
                    return;
                }

                if (!_pendingExpectedDirectMethodReadyIds.TryRemove(requestId.Value, out ByteString? expectedReadyId))
                {
                    Trace.TraceWarning("Received direct method exec message with unknown request Id. Ignoring it.");
                    return;
                }

                if (!_pendingExpectedDirectMethodNames.TryRemove(requestId.Value, out string? methodName))
                {
                    // This should never happen since _pendingExpectedDirectMethodReadyIds and _pendingExpectedDirectMethodNames should have the same set of keys.
                    Trace.TraceWarning("Received direct method exec message, but could not correlate its method name to a previously received probe message. Ignoring it.");
                    return;
                }

                uint responseTimeoutInSeconds = publish.MessageExpiryInterval;
                Stopwatch stopwatch = Stopwatch.StartNew();

                Exec exec = Exec.Parser.ParseFrom(publish.Payload);

                if (!expectedReadyId.Equals(exec.ReadyId))
                {
                    Trace.TraceWarning("Received direct method exec message with unexpected ready Id. Ignoring it.");
                    return;
                }

                DirectMethodRequestReceivedEventArgs directMethodInvokedArgs = new()
                {
                    MethodName = methodName,
                    Payload = exec.Params.Span.ToArray()
                };

                DirectMethodResponse methodResponse = await DirectMethodInvokedAsync.Invoke(directMethodInvokedArgs);

                stopwatch.Stop();

                if (methodResponse == null)
                {
                    // This is a special case that is mostly reserved for the unified direct method client to communicate back to this client that no handler was registered, so no response should be sent by this client.
                    return;
                }

                uint secondsSinceReceivingExec;
                try
                {
                    secondsSinceReceivingExec = (uint)stopwatch.Elapsed.TotalSeconds;
                }
                catch (InvalidCastException)
                {
                    // Should only happen if the double grows so large that a uint can't contain it.
                    Trace.TraceError("Could not calculate time since exec message was received. This likely means the 'DirectMethodInvokedAsync' callback took too long. Cannot send result, so discarding this probe message");
                    return;
                }

                uint remainingResponseTimeoutInSeconds = responseTimeoutInSeconds - secondsSinceReceivingExec;

                if (remainingResponseTimeoutInSeconds < 1)
                {
                    Trace.TraceWarning("Application did not respond to direct method exec message before the response timeout elapsed, so no result message will be sent.");
                    return;
                }

                Result result = new()
                {
                    Status = methodResponse.Status,
                    Body = methodResponse.Payload is null
                        ? ByteString.Empty
                        : ByteString.CopyFrom(methodResponse.Payload)
                };

                MqttPublish resultPublish = new()
                {
                    Topic = string.Format("ih/{0}/srv/methods", currentConnectionContext.DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    Payload = result.ToByteArray(),
                    CorrelationData = publish.CorrelationData,
                    MessageExpiryInterval = remainingResponseTimeoutInSeconds,
                    ContentType = ProtobufContentType
                };

                resultPublish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("result:1")));

                MqttPublishAck puback = await _connection.MqttClient.PublishAsync(resultPublish);

                if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceError("Failed to send the response to a direct method exec because the MQTT broker rejected the publish with reason code {0} and reason string {1}", puback.ReasonCode, puback.ReasonString);
                }
            }
        }

        public void Dispose()
        {
            _connection.MqttClient.PublishReceivedAsync -= HandleReceivedAzureEventGridMqttPublish;
        }
    }
}
