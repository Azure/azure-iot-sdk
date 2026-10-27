// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods;
using Microsoft.Azure.Iot.Device.Events;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Text.RegularExpressions;
using System.Web;
using Microsoft.Azure.Iot.Device.Unified.Models;

namespace Microsoft.Azure.Iot.Device.Unified.DirectMethods
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

        private MQTTv5.DirectMethods.DirectMethodClient _mqttv5DirectMethodClient;

        private readonly SingleHandlerEvent<Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>> _directMethodInvoked =
            new(nameof(DirectMethodInvokedAsync));

        /// <summary>
        /// An event that executes whenever this device receives a direct method request from IoT hub. After executing the direct method, the device must
        /// provide a direct method response.
        /// </summary>
        /// <remarks>
        /// Only one handler can be added at a time because only one direct method response can be sent per request.
        /// </remarks>
        /// <exception cref="InvalidOperationException">Thrown when adding a handler while another handler is already added.</exception>
        public event Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? DirectMethodInvokedAsync
        {
            add => _directMethodInvoked.Add(value);
            remove => _directMethodInvoked.Remove(value);
        }

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
        /// <exception cref="InvalidOperationException">Thrown if <paramref name="connection"/> opted out of direct methods (see <see cref="ConnectionClientOptions.EnableDirectMethods"/>).</exception>
        public DirectMethodClient(IConnectionClient connection)
        {
            if (!connection.AreDirectMethodsEnabled)
            {
                throw new InvalidOperationException($"The provided connection client opted out of direct methods. Set {nameof(ConnectionClientOptions)}.{nameof(ConnectionClientOptions.EnableDirectMethods)} to true to use a {nameof(DirectMethodClient)}.");
            }

            _connection = connection;
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;

            _mqttv5DirectMethodClient = new(new Stub(_connection));
            _mqttv5DirectMethodClient.DirectMethodProbeReceivedAsync += HandleMqttv5DirectMethodProbeRequestAsync;
            _mqttv5DirectMethodClient.DirectMethodInvokedAsync += HandleMqttv5DirectMethodRequestAsync;
        }

        private async Task<DirectMethodResponse> HandleMqttv5DirectMethodRequestAsync(DirectMethodRequestReceivedEventArgs args)
        {
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? handler = _directMethodInvoked.Handler;
            if (handler == null)
            {
                Trace.TraceError("Received a direct method request, but no handler was set on this client to handle it.");
#pragma warning disable CS8600 // Converting null literal or possible null value to non-nullable type.
#pragma warning disable CS8603 // Possible null reference return.
                return (DirectMethodResponse)null; // A little hacky, but we want a way for the unified client to communicate back to the MQTTv5 client to not send anything in response
#pragma warning restore CS8603 // Possible null reference return.
#pragma warning restore CS8600 // Converting null literal or possible null value to non-nullable type.
            }

            return await handler.Invoke(args);
        }

        private Task<DirectMethodProbeAck> HandleMqttv5DirectMethodProbeRequestAsync(DirectMethodRequestProbeReceivedEventArgs args)
        {
            if (_directMethodInvoked.Handler == null)
            {
                Trace.TraceError("Received a direct method probe, but no handler was set on this client to handle it, so it is rejecting the probe as method not found.");
                return Task.FromResult(DirectMethodProbeAck.Rejected(RejectedReason.MethodNotFound));
            }

            // Since Classic Hub has no concept of a direct method probe message, make this unified client just accept any received probe request
            return Task.FromResult(DirectMethodProbeAck.Accepted());
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (!args.Publish.Topic.StartsWith("$iothub/methods/"))
            {
                // The publish is not relevant to this client, so ignore it. This check needs to happen prior to checking the deviceId within the topic b/c deviceId is
                // not available until after provisioning finishes and this client may be setup prior to provisioning. This allows this client to ignore DPS
                // publishes without needing to know the deviceId.
                return;
            }

            var currentConnectionContext = _connection.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                // This should never happen barring some race condition?
                Trace.TraceWarning("Received a direct method request, but the connection was lost. Ignoring it.");
                return;
            }

            if (currentConnectionContext.ConnectionProfile == IotHubConnectionType.Mqttv5)
            {
                // The underlying MQTTv5 DirectMethodClient handles this flow
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

            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? handler = _directMethodInvoked.Handler;
            if (handler == null)
            {
                Trace.TraceError("Received a direct method request, but no handler was set on this client to handle it.");
                return;
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

            DirectMethodResponse methodResponse = await handler.Invoke(methodRequest);

            string responsePublishTopic = string.Format(ClassicDirectMethodsResponseTopicFormat, methodResponse.Status, requestId);
            MqttPublish publish = new MqttPublish()
            {
                Topic = responsePublishTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            if (methodResponse.Payload != null)
            {
                publish.Payload = methodResponse.Payload;
            }

            MqttPublishAck puback = await _connection.PublishAsync(publish, throwOnErrorReasonCode: true, CancellationToken.None);

            if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
            {
                Trace.TraceError("Failed to send the response to a direct method because the MQTT broker rejected the publish with reason code {0} and reason string {1}", puback.ReasonCode, puback.ReasonString);
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;

            _mqttv5DirectMethodClient.DirectMethodProbeReceivedAsync -= HandleMqttv5DirectMethodProbeRequestAsync;
            _mqttv5DirectMethodClient.DirectMethodInvokedAsync -= HandleMqttv5DirectMethodRequestAsync;

            if (disposing)
            {
                _connection.Dispose();
            }
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;

            _mqttv5DirectMethodClient.DirectMethodProbeReceivedAsync -= HandleMqttv5DirectMethodProbeRequestAsync;
            _mqttv5DirectMethodClient.DirectMethodInvokedAsync -= HandleMqttv5DirectMethodRequestAsync;

            _connection.Dispose();
        }
    }
}
