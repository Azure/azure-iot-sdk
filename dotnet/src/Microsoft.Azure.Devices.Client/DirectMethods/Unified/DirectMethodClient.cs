using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Connection.Unified;
using Microsoft.Azure.Devices.Client.DirectMethods.Gen2;
using Microsoft.Azure.Devices.Client.DirectMethods.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Text.RegularExpressions;
using System.Web;

namespace Microsoft.Azure.Devices.Client.DirectMethods.Unified
{
    /// <summary>
    /// A feature client for receiving and responding to direct method requests from IoT Hub.
    /// </summary>
    public class DirectMethodClient : IDisposable
    {
        internal const string ClassicDirectMethodsRequestTopic = "$iothub/methods/POST/";
        private const string ClassicDirectMethodsResponseTopicFormat = "$iothub/methods/res/{0}/?$rid={1}";
        private const string RequestIdTopicKey = "$rid";

        private IConnectionClient _connection;

        private Client.DirectMethods.Gen2.DirectMethodClient _aegDirectMethodClient;

        /// <summary>
        /// An event that executes whenever this device receives a direct method request from IoT hub. After executing the direct method, the device must
        /// provide a direct method response.
        /// </summary>
        public event Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? DirectMethodInvokedAsync;

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
        /// directMethodClient.DirectMethodInvokedAsync += SomeDirectMethodHandlingMethod;
        /// 
        /// // Open the connection (and start receiving direct methods)
        /// await connectionClient.ProvisionAndConnectAsync();
        /// </code>
        /// </example>
        public DirectMethodClient(IConnectionClient connection)
        {
            _connection = connection;
            _connection.MqttClient.PublishReceivedAsync += HandleReceivedClassicMqttPublish;
            
            _aegDirectMethodClient = new(_connection);
            _aegDirectMethodClient.DirectMethodProbeReceivedAsync += HandleAegDirectMethodProbeRequestAsync;
            _aegDirectMethodClient.DirectMethodInvokedAsync += HandleAegDirectMethodRequestAsync;
        }

        private async Task<DirectMethodResponse> HandleAegDirectMethodRequestAsync(DirectMethodRequestReceivedEventArgs args)
        {
            if (DirectMethodInvokedAsync == null)
            {
                Trace.TraceError("Received a direct method request, but no handler was set on this client to handle it.");
#pragma warning disable CS8600 // Converting null literal or possible null value to non-nullable type.
#pragma warning disable CS8603 // Possible null reference return.
                return (DirectMethodResponse)null; // A little hacky, but we want a way for the unified client to communicate back to the Gen2 client to not send anything in response
#pragma warning restore CS8603 // Possible null reference return.
#pragma warning restore CS8600 // Converting null literal or possible null value to non-nullable type.
            }

            return await DirectMethodInvokedAsync.Invoke(args);
        }

        private Task<ProbeAck> HandleAegDirectMethodProbeRequestAsync(DirectMethodRequestProbeReceivedEventArgs args)
        {
            // Since Classic Hub has no concept of a direct method probe message, make this unified client just accept any received probe request
            return Task.FromResult(new ProbeAck()
            {
                Ready = new()
                { 
                    ReadyId = ByteString.CopyFrom(Guid.NewGuid().ToByteArray()) //TODO why should user have to provide this? We do all the correlation internally. Is that justification enough to wrap this proto class?
                }
            });
        }

        private async Task HandleReceivedClassicMqttPublish(MqttPublishReceivedEventArgs args)
        {
            var currentConnectionContext = _connection.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                // This should never happen barring some race condition?
                Trace.TraceWarning("Received a direct method request, but the connection was lost. Ignoring it.");
                return;
            }

            if (currentConnectionContext.IsAzureEventGrid)
            {
                // The underlying Gen2 DirectMethodClient handles this flow
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
                Trace.TraceError("Received a direct method request, but no handler was set on this client to handle it.");            
            }

            byte[] payload = args.Publish.Payload;

            string[] tokens = Regex.Split(args.Publish.Topic, "/", RegexOptions.Compiled);

            NameValueCollection queryStringKeyValuePairs = HttpUtility.ParseQueryString(tokens[4]);
            string? requestId = queryStringKeyValuePairs.Get(RequestIdTopicKey);
            if (requestId == null)
            {
                Trace.TraceError("Received a malformed direct method request. Ignoring it.");
            }

            string methodName = tokens[3];

            var methodRequest = new DirectMethodRequestReceivedEventArgs()
            {
                Payload = payload,
                MethodName = methodName,
            };

            DirectMethodResponse methodResponse = await DirectMethodInvokedAsync!.Invoke(methodRequest);

            string responsePublishTopic = string.Format(ClassicDirectMethodsResponseTopicFormat,methodResponse.Status, requestId);
            MqttPublish publish = new MqttPublish()
            {
                Topic = responsePublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            if (methodResponse.Payload != null)
            {
                publish.Payload = methodResponse.Payload;
            }

            MqttPublishAck puback = await _connection.MqttClient.PublishAsync(publish, CancellationToken.None);

            if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
            {
                Trace.TraceError("Failed to send the response to a direct method because the MQTT broker rejected the publish with reason code {0} and reason string {1}", puback.ReasonCode, puback.ReasonString);
            }
        }

        public void Dispose()
        {
            _connection.MqttClient.PublishReceivedAsync -= HandleReceivedClassicMqttPublish;

            _aegDirectMethodClient.DirectMethodProbeReceivedAsync -= HandleAegDirectMethodProbeRequestAsync;
            _aegDirectMethodClient.DirectMethodInvokedAsync -= HandleAegDirectMethodRequestAsync;
        }
    }
}
