using Google.Protobuf;
using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Collections.Specialized;
using System.Text;
using System.Text.RegularExpressions;
using System.Web;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodClient
    {
        internal const string ClassicDirectMethodsRequestTopic = "$iothub/methods/POST/";
        private const string ClassicDirectMethodsResponseTopicFormat = "$iothub/methods/res/{0}/?$rid={1}";
        private const string RequestIdTopicKey = "$rid";

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
            _connection.ApplicationMessageReceivedAsync += HandleReceivedMqttPublish;
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

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {
                if (DirectMethodInvokedAsync != null && DirectMethodProbeReceivedAsync != null && args.Publish.Topic.Equals(string.Format("ih/{deviceId}/dev/methods", _connection.CurrentConnectionContext.DeviceId)))
                {
                    MqttPublish publish = args.Publish;
                    if (publish.UserProperties.TryGetType(out string? directMethodMessageType, out int? directMethodMessageTypeValue))
                    {
                        if (directMethodMessageType.Equals("probe"))
                        {
                            if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
                            {
                                return; // Malformed request. Discard it silently
                            }

                            Probe probe = Probe.Parser.ParseFrom(publish.PayloadAsByteArray);

                            ProbeAck probeAck = await DirectMethodProbeReceivedAsync.Invoke(new() { Probe = probe });

                            uint remainingConnectTimeout = 100; // TODO how is this derived?

                            MqttPublish probeAckPublish = new()
                            {
                                Topic = string.Format("ih/{deviceId}/srv/methods", _connection.CurrentConnectionContext.DeviceId),
                                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                                PayloadAsByteArray = probeAck.ToByteArray(),
                                CorrelationData = publish.CorrelationData,
                                MessageExpiryInterval = remainingConnectTimeout,
                            };

                            probeAckPublish.UserProperties.Add(new() { Name = "type", Value = Encoding.UTF8.GetBytes(string.Format("probe-ack:{}", directMethodMessageTypeValue)) });
                        }
                        else if (directMethodMessageType.Equals("exec"))
                        {
                            if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
                            {
                                return; // Malformed request. Discard it silently
                            }
                        }
                        else
                        {
                            //TODO unrecognized type
                        }
                    }
                }
            }
            else
            {
                // Parse and respond to the direct method according the classic direct method mqtt communication pattern
                //
                // Note that all direct method invocation messages are QoS 0, so no need to ack the MQTT message here
                if (DirectMethodInvokedAsync != null && args.Publish.Topic.StartsWith(ClassicDirectMethodsRequestTopic))
                {
                    byte[] payload = args.Publish.PayloadAsByteArray;

                    string[] tokens = Regex.Split(args.Publish.Topic, "/", RegexOptions.Compiled);

                    NameValueCollection queryStringKeyValuePairs = HttpUtility.ParseQueryString(tokens[4]);
                    string? requestId = queryStringKeyValuePairs.Get(RequestIdTopicKey);
                    if (requestId == null)
                    {
                        throw new Exception("TODO");
                    }

                    string methodName = tokens[3];

                    var methodRequest = new DirectMethodRequest()
                    {
                        Payload = payload,
                        MethodName = methodName,
                        RequestId = requestId,
                    };

                    DirectMethodResponse methodResponse = await DirectMethodInvokedAsync.Invoke(new() { Request = methodRequest });

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
            }
        }
    }
}
