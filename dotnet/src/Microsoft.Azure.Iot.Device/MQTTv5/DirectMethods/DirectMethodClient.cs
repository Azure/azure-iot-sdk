// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Events;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;

namespace Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods
{
    /// <summary>
    /// A feature client for receiving and responding to direct method requests from IoT Hub.
    /// </summary>
    public class DirectMethodClient : IDisposable
    {
        //TODO need to think more on connection loss scenario. If direct method is received and is mid-processing when connection is lost, what should happen? Is sending response publish upon reconnect enough?
        private const string ProtobufContentType = "application/protobuf";

        // The connection client substitutes this device's current ID for the placeholder when it sends each response, so a
        // response still goes to the right topic if the device ID changes (for example, after re-provisioning).
        private const string DirectMethodsResponseTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/methods";
        private const string DirectMethodsRequestTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/methods";

        // Only applicable for MQTTv5 Hub scenario. Maps from request Id (GUID) to ready Id (also GUID). When this client receives an Exec message, it should only notify the user about it
        // if this map contains the exec's request Id and it maps to the ready Id in the Exec message.
        private ConcurrentDictionary<Guid, ByteString> _pendingExpectedDirectMethodReadyIds = new();

        // Only applicable for MQTTv5 Hub scenario. Maps from request Id (GUID) to method name.
        private ConcurrentDictionary<Guid, string> _pendingExpectedDirectMethodNames = new();

        private IConnectionClient _connection;

        private readonly SingleHandlerEvent<Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>> _directMethodInvoked =
            new(nameof(DirectMethodInvokedAsync));
        private readonly SingleHandlerEvent<Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>>> _directMethodProbeReceived =
            new(nameof(DirectMethodProbeReceivedAsync));

        /// <summary>
        /// An event that executes whenever this device receives a direct method request from IoT hub. After executing the direct method, the device must
        /// provide a direct method response.
        /// </summary>
        /// <remarks>
        /// Only one handler can be added at a time because only one direct method response can be sent per request.
        /// </remarks>
        /// <exception cref="InvalidOperationException">Thrown when adding a handler while another handler is already added, or adding a multicast delegate.</exception>
        public event Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? DirectMethodInvokedAsync
        {
            add => _directMethodInvoked.Add(value);
            remove => _directMethodInvoked.Remove(value);
        }

        /// <summary>
        /// An event that executes whenever this device receives a direct method probe from IoT hub. This event is a precursor to receiving the direct method itself 
        /// and allows your application to decide whether it is ready to receive this direct method or not.
        /// </summary>
        /// <remarks>
        /// This feature is only supported by IoT hubs that use MQTTv5. Older IoT hubs will never send this probe.
        /// Only one handler can be added at a time because only one probe response can be sent per request.
        /// </remarks>
        /// <exception cref="InvalidOperationException">Thrown when adding a handler while another handler is already added, or adding a multicast delegate.</exception>
        public event Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>>? DirectMethodProbeReceivedAsync
        {
            add => _directMethodProbeReceived.Add(value);
            remove => _directMethodProbeReceived.Remove(value);
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
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (!args.Publish.Topic.StartsWith("ih/") || !args.Publish.Topic.EndsWith("/dev/methods"))
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

            if (!args.Publish.Topic.Equals(DirectMethodsRequestTopic.Replace(IConnectionClient.DeviceIdPlaceholder, currentConnectionContext.DeviceId, StringComparison.Ordinal)))
            {
                // Message isn't relevant to this device. Ignore it.
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

            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? methodHandler = _directMethodInvoked.Handler;
            Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>>? probeHandler = _directMethodProbeReceived.Handler;

            if (directMethodMessageType!.Equals("probe"))
            {
                if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
                {
                    return; // Malformed request. Discard it silently
                }

                uint connectTimeoutRemainingUponReceivingProbe = publish.MessageExpiryInterval;
                Stopwatch stopwatch = Stopwatch.StartNew();

                Probe probe = Probe.Parser.ParseFrom(publish.Payload);

                DirectMethodProbeAck probeAck;
                if (methodHandler == null || probeHandler == null)
                {
                    // Without both callbacks this client can't execute the method, so tell the service rather than letting the probe time out.
                    Trace.TraceError("Received a direct method probe, but this client has not registered both callbacks, so it is rejecting the probe as method not found.");
                    probeAck = DirectMethodProbeAck.Rejected(RejectedReason.MethodNotFound);
                }
                else
                {
                    probeAck = await probeHandler.Invoke(new() { MethodName = probe.MethodName, ResponseTimeoutSeconds = probe.ResponseTimeoutSeconds });
                }

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
                    Topic = DirectMethodsResponseTopic,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    Payload = probeAck.ProbeAck.ToByteArray(),
                    CorrelationData = publish.CorrelationData,
                    MessageExpiryInterval = remainingConnectTimeoutInSeconds,
                    ContentType = ProtobufContentType
                };

                probeAckPublish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("probe-ack:1")));

                if (probeAck.ProbeAck.Ready != default)
                {
                    // The user signalled that the device was ready for the direct method, so locally save the ready Id and request Id
                    _pendingExpectedDirectMethodReadyIds.TryAdd(requestId.Value, probeAck.ProbeAck.Ready.ReadyId);
                    _pendingExpectedDirectMethodNames.TryAdd(requestId.Value, probe.MethodName);
                }

                MqttPublishAck puback = await _connection.PublishAsync(probeAckPublish);

                if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceError("Failed to send the response to a direct method probe because the MQTT broker rejected the publish with reason code {0} and reason string {1}", puback.ReasonCode, puback.ReasonString);
                }
            }
            else if (directMethodMessageType.Equals("exec"))
            {
                if (methodHandler == null)
                {
                    Trace.TraceError("Received a direct method exec message, but this client has no direct method callback registered, so it cannot be handled.");
                    return;
                }

                if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
                {
                    Trace.TraceWarning("Received direct method exec message with malformed request Id. Ignoring it.");
                    return;
                }

                if (!_pendingExpectedDirectMethodReadyIds.ContainsKey(requestId.Value))
                {
                    Trace.TraceWarning("Received direct method exec message with unknown request Id. Ignoring it.");
                    return;
                }

                uint responseTimeoutInSeconds = publish.MessageExpiryInterval;
                Stopwatch stopwatch = Stopwatch.StartNew();
                using CancellationTokenSource responseTimeoutCts = new(TimeSpan.FromSeconds(responseTimeoutInSeconds));

                // Parse before removing the pending request so that a malformed exec leaves this client ready to receive a well-formed
                // exec for the same request.
                Exec exec;
                try
                {
                    exec = Exec.Parser.ParseFrom(publish.Payload);
                }
                catch (InvalidProtocolBufferException ex)
                {
                    Trace.TraceError("Received direct method exec message for request Id {0} with a malformed payload. Ignoring it and continuing to wait for a valid exec message for this request. Error: {1}", requestId.Value, ex.Message);
                    return;
                }

                if (!_pendingExpectedDirectMethodReadyIds.TryRemove(requestId.Value, out ByteString? expectedReadyId))
                {
                    // A concurrently handled exec for the same request already claimed it.
                    Trace.TraceWarning("Received direct method exec message with unknown request Id. Ignoring it.");
                    return;
                }

                if (!_pendingExpectedDirectMethodNames.TryRemove(requestId.Value, out string? methodName))
                {
                    // This should never happen since _pendingExpectedDirectMethodReadyIds and _pendingExpectedDirectMethodNames should have the same set of keys.
                    Trace.TraceWarning("Received direct method exec message, but could not correlate its method name to a previously received probe message. Ignoring it.");
                    return;
                }

                if (!expectedReadyId.Equals(exec.ReadyId))
                {
                    Trace.TraceWarning("Received direct method exec message with unexpected ready Id. Ignoring it.");
                    return;
                }

                DirectMethodRequestReceivedEventArgs directMethodInvokedArgs = new()
                {
                    MethodName = methodName,
                    Payload = exec.Params.Span.ToArray(),
                    CancellationToken = responseTimeoutCts.Token
                };

                DirectMethodResponse methodResponse;
                try
                {
                    methodResponse = await methodHandler.Invoke(directMethodInvokedArgs);
                }
                catch (OperationCanceledException) when (responseTimeoutCts.IsCancellationRequested)
                {
                    Trace.TraceWarning("The direct method with name {0} took longer than the remaining direct method response timeout, so no result message will be sent.", methodName);
                    return;
                }

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
                    Topic = DirectMethodsResponseTopic,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                    Payload = result.ToByteArray(),
                    CorrelationData = publish.CorrelationData,
                    MessageExpiryInterval = remainingResponseTimeoutInSeconds,
                    ContentType = ProtobufContentType
                };

                resultPublish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("result:1")));

                MqttPublishAck puback = await _connection.PublishAsync(resultPublish);

                if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceError("Failed to send the response to a direct method exec because the MQTT broker rejected the publish with reason code {0} and reason string {1}", puback.ReasonCode, puback.ReasonString);
                }
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
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
            _connection.Dispose();
        }
    }
}
