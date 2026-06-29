using Google.Protobuf;
using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Collections.Concurrent;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;
using System.Web;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodClient : IDisposable
    {
        internal const string ClassicDirectMethodsRequestTopic = "$iothub/methods/POST/";
        private const string ClassicDirectMethodsResponseTopicFormat = "$iothub/methods/res/{0}/?$rid={1}";
        private const string RequestIdTopicKey = "$rid";

        // Only applicable for AEG Hub scenario. Maps from request Id (GUID) to ready Id (also GUID). When this client receives an Exec message, it should only notify the user about it
        // if this map contains the exec's request Id and it maps to the ready Id in the Exec message.
        private ConcurrentDictionary<Guid, ByteString> _pendingExpectedDirectMethodReadyIds = new();

        // Only applicable for AEG Hub scenario. Maps from request Id (GUID) to method name.
        private ConcurrentDictionary<Guid, string> _pendingExpectedDirectMethodNames = new();

        private ConnectionClient _connection;

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

        public DirectMethodClient(ConnectionClient connection)
        {
            _connection = connection;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedAzureEventGridMqttPublish;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedClassicMqttPublish;
            _connection.DisconnectedAsync += HandleDisconnectionAsync;
            _connection.ConnectedAsync += HandleConnectionAsync;
        }

        private async void HandleConnectionAsync(MqttClientConnectedEventArgs args)
        {
        }

        private async void HandleDisconnectionAsync(MqttClientDisconnectedEventArgs args)
        {
            // no idea how a disconnection should impact an ongoing direct method
        }

        private async Task HandleReceivedAzureEventGridMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (!_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {
                // The other handler covers this scenario
                return;
            }

            if (!args.Publish.Topic.Equals(string.Format("ih/{deviceId}/dev/methods", _connection.CurrentConnectionContext.DeviceId)))
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
            if (publish.UserProperties.TryGetType(out string? directMethodMessageType, out int? directMethodMessageTypeValue))
            {
                Trace.TraceWarning("Received a direct method message, but it is either missing the message type or the message type is malformed. Ignoring it.");
                return;
            }

            // All AEG Hub direct method messages are QoS 1, so ack immediately upon recognizing the message as one
            await args.AcknowledgeAsync(CancellationToken.None);

            if (directMethodMessageType!.Equals("probe") && directMethodMessageTypeValue == 1)
            {
                if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
                {
                    return; // Malformed request. Discard it silently
                }

                uint connectTimeoutRemainingUponReceivingProbe = publish.MessageExpiryInterval;
                Stopwatch stopwatch = Stopwatch.StartNew();

                Probe probe = Probe.Parser.ParseFrom(publish.PayloadAsByteArray);

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
                    Topic = string.Format("ih/{deviceId}/srv/methods", _connection.CurrentConnectionContext.DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    PayloadAsByteArray = probeAck.ToByteArray(),
                    CorrelationData = publish.CorrelationData,
                    MessageExpiryInterval = remainingConnectTimeoutInSeconds,
                };

                probeAckPublish.UserProperties.Add(new() { Name = "type", Value = Encoding.UTF8.GetBytes(string.Format("probe-ack:1")) });

                if (probeAck.Ready != default)
                {
                    // The user signalled that the device was ready for the direct method, so locally save the ready Id and request Id
                    _pendingExpectedDirectMethodReadyIds.TryAdd(requestId.Value, probeAck.Ready.ReadyId);
                    _pendingExpectedDirectMethodNames.TryAdd(requestId.Value, probe.MethodName);
                }

                await _connection.PublishAsync(probeAckPublish);
            }
            else if (directMethodMessageType.Equals("exec") && directMethodMessageTypeValue == 1)
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

                Exec exec = Exec.Parser.ParseFrom(publish.PayloadAsByteArray);

                if (!expectedReadyId.Equals(exec.ReadyId))
                {
                    Trace.TraceWarning("Received direct method exec message with unexpected ready Id. Ignoring it.");
                    return;
                }

                DirectMethodRequestReceivedEventArgs directMethodInvokedArgs = new()
                {
                    MethodName = methodName,
                    RequestId = requestId.Value.ToString(),
                    Payload = exec.Params.Span.ToArray()
                };

                DirectMethodResponse methodResponse = await DirectMethodInvokedAsync.Invoke(directMethodInvokedArgs);

                Result result = Result.Parser.ParseFrom(methodResponse.Payload);
                result.Status = methodResponse.Status;

                MqttPublish resultPublish = new()
                {
                    Topic = string.Format("ih/{deviceId}/srv/methods", _connection.CurrentConnectionContext.DeviceId),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    PayloadAsByteArray = result.ToByteArray(),
                    CorrelationData = publish.CorrelationData,
                    //MessageExpiryInterval = remainingConnectTimeout, //TODO
                };

                resultPublish.UserProperties.Add(new() { Name = "type", Value = Encoding.UTF8.GetBytes(string.Format("result:1")) });

                await _connection.PublishAsync(resultPublish);
            }
        }

        private async Task HandleReceivedClassicMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {
                // The other handler covers this scenario
                return;
            }

            // Parse and respond to the direct method according the classic direct method mqtt communication pattern
            //
            // Note that all direct method invocation messages are QoS 0, so no need to ack the MQTT message here
            if (!args.Publish.Topic.StartsWith(ClassicDirectMethodsRequestTopic))
            {
                // The message isn't relevant to this client
                return;
            }

            if (DirectMethodInvokedAsync == null)
            {
                Trace.TraceError("Received a direct method message, but no handler was set on this client to handle it.");            
            }

            byte[] payload = args.Publish.PayloadAsByteArray;

            string[] tokens = Regex.Split(args.Publish.Topic, "/", RegexOptions.Compiled);

            NameValueCollection queryStringKeyValuePairs = HttpUtility.ParseQueryString(tokens[4]);
            string? requestId = queryStringKeyValuePairs.Get(RequestIdTopicKey);
            if (requestId == null)
            {
                throw new Exception("TODO");
            }

            string methodName = tokens[3];

            var methodRequest = new DirectMethodRequestReceivedEventArgs()
            {
                Payload = payload,
                MethodName = methodName,
                RequestId = requestId,
            };

            DirectMethodResponse methodResponse = await DirectMethodInvokedAsync!.Invoke(methodRequest);

            string responsePublishTopic = string.Format(ClassicDirectMethodsResponseTopicFormat,methodResponse.Status, requestId);
            MqttPublish publish = new MqttPublish()
            {
                Topic = responsePublishTopic,
                PayloadAsByteArray = payload,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            MqttPublishAck result = await _connection.PublishAsync(publish, CancellationToken.None);

            if (result.ReasonCode != MqttClientPublishReasonCode.Success)
            {
                throw new Exception("TODO");
            }
        }

        public void Dispose()
        {
            _connection.ApplicationMessageReceivedAsync -= HandleReceivedAzureEventGridMqttPublish;
            _connection.ApplicationMessageReceivedAsync -= HandleReceivedClassicMqttPublish;
            _connection.DisconnectedAsync -= HandleDisconnectionAsync;
            _connection.ConnectedAsync -= HandleConnectionAsync;
        }
    }
}
