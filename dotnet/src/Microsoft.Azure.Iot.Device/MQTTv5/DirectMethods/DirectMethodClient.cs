// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Events;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Models;
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

        // Maps from request Id to the ready token this client sent for it. When this client receives an Exec message, it should only notify the user about it
        // if this map contains the exec's request Id and its token has the ready Id in the Exec message. Whoever removes a token from this map owns its
        // capacity slot: the exec handler keeps it until the method handler returns, and every other path releases it.
        private readonly ConcurrentDictionary<Guid, ReadyToken> _readyTokens = new();

        // Ready tokens plus executing method handlers. Never exceeds _maxConcurrentDirectMethods.
        private int _activeDirectMethods;
        private readonly int _maxConcurrentDirectMethods;
        private readonly TimeProvider _timeProvider;

        // Request Ids of probes currently being answered, so only one delivery of a request is admitted at a time.
        private readonly ConcurrentDictionary<Guid, byte> _probesInProgress = new();

        // The longest due time System.Threading.Timer accepts.
        private const double MaxTimerDueTimeMilliseconds = uint.MaxValue - 1.0;

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
        /// If the handler accepts a probe while this client already holds <see cref="DirectMethodClientOptions.MaxConcurrentDirectMethods"/>
        /// invocations, this client sends a <see cref="RejectedReason.DeviceBusy"/> rejection instead.
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
            : this(connection, new DirectMethodClientOptions())
        {
        }

        /// <summary>
        /// Construct a new <see cref="DirectMethodClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <param name="options">The options for this client, such as how many direct method invocations it holds at once.</param>
        /// <exception cref="ArgumentNullException">Thrown if <paramref name="options"/> is null.</exception>
        /// <exception cref="ArgumentOutOfRangeException">Thrown if <see cref="DirectMethodClientOptions.MaxConcurrentDirectMethods"/> is less than 1.</exception>
        public DirectMethodClient(IConnectionClient connection, DirectMethodClientOptions options)
            : this(connection, options, TimeProvider.System)
        {
        }

        internal DirectMethodClient(IConnectionClient connection, DirectMethodClientOptions options, TimeProvider timeProvider)
        {
            ArgumentNullException.ThrowIfNull(options);
            ArgumentOutOfRangeException.ThrowIfLessThan(options.MaxConcurrentDirectMethods, 1, $"{nameof(options)}.{nameof(options.MaxConcurrentDirectMethods)}");

            _maxConcurrentDirectMethods = options.MaxConcurrentDirectMethods;
            _timeProvider = timeProvider;
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

                // Only one delivery of a request is admitted at a time, so overlapping redeliveries can't send conflicting probe acks.
                if (!_probesInProgress.TryAdd(requestId.Value, 0))
                {
                    // A redelivery of a probe this client is still answering.
                    return;
                }

                try
                {
                    // Checked only while holding the gate, so the lookup can't be stale relative to a delivery that just accepted it.
                    if (_readyTokens.ContainsKey(requestId.Value))
                    {
                        // A redelivery of a probe this client already accepted. The original probe ack has its own QoS 1 delivery lifecycle.
                        return;
                    }

                    await HandleProbeAsync(publish, requestId.Value, methodHandler, probeHandler);
                }
                finally
                {
                    _probesInProgress.TryRemove(requestId.Value, out _);
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

                if (!_readyTokens.ContainsKey(requestId.Value))
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

                if (!_readyTokens.TryRemove(requestId.Value, out ReadyToken? readyToken))
                {
                    // A concurrently handled exec for the same request, or the ready wait timer, already claimed it.
                    Trace.TraceWarning("Received direct method exec message with unknown request Id. Ignoring it.");
                    return;
                }

                // This exec now owns the token's capacity slot, which it releases once the method handler returns.
                readyToken.Dispose();
                string methodName = readyToken.MethodName;

                if (!readyToken.ReadyId.Equals(exec.ReadyId))
                {
                    ReleaseCapacity();
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
                finally
                {
                    // The invocation stops counting against the capacity limit once the handler returns control, even if it ran past its deadline.
                    ReleaseCapacity();
                }

                stopwatch.Stop();

                if (methodResponse == null)
                {
                    // This is a special case that is mostly reserved for the unified direct method client to communicate back to this client that no handler was registered, so no response should be sent by this client.
                    return;
                }

                double secondsSinceReceivingExec = Math.Floor(stopwatch.Elapsed.TotalSeconds);

                uint remainingResponseTimeoutInSeconds = secondsSinceReceivingExec >= responseTimeoutInSeconds
                    ? 0
                    : responseTimeoutInSeconds - (uint)secondsSinceReceivingExec;

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

        private async Task HandleProbeAsync(
            MqttPublish publish,
            Guid requestId,
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? methodHandler,
            Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>>? probeHandler)
        {
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

            double secondsSinceReceivingProbe = Math.Floor(stopwatch.Elapsed.TotalSeconds);

            // Saturate at zero: a probe handler that outlived the connect budget must not wrap the unsigned remainder.
            uint remainingConnectTimeoutInSeconds = secondsSinceReceivingProbe >= connectTimeoutRemainingUponReceivingProbe
                ? 0
                : connectTimeoutRemainingUponReceivingProbe - (uint)secondsSinceReceivingProbe;

            if (remainingConnectTimeoutInSeconds < 1)
            {
                Trace.TraceWarning("Application did not respond to direct method probe message before the connect timeout elapsed, so no probe ack message will be sent.");
                return;
            }

            ReadyToken? readyToken = null;
            if (probeAck.ProbeAck.Ready != default)
            {
                if (TryReserveCapacity())
                {
                    // The user signalled that the device was ready for the direct method, so locally save the ready Id and request Id
                    // until the matching exec arrives or the ready wait budget expires.
                    readyToken = new ReadyToken(requestId, probeAck.ProbeAck.Ready.ReadyId, probe.MethodName);

                    // Create the timer disarmed and arm it only once the token is registered, so it can't fire before there is a token
                    // for it to release.
                    readyToken.ReadyWaitTimer = _timeProvider.CreateTimer(OnReadyWaitExpired, readyToken, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);

                    if (!_readyTokens.TryAdd(requestId, readyToken))
                    {
                        // Unreachable while _probesInProgress admits one delivery per request, but never leak the slot.
                        readyToken.Dispose();
                        ReleaseCapacity();
                        return;
                    }

                    ArmReadyWaitTimer(readyToken, ReadyWaitBudget(remainingConnectTimeoutInSeconds, probe.ResponseTimeoutSeconds));
                }
                else
                {
                    Trace.TraceWarning(
                        "Rejecting a direct method probe for method {0} as device busy because this client already holds its maximum of {1} concurrent direct methods.",
                        probe.MethodName,
                        _maxConcurrentDirectMethods);
                    probeAck = DirectMethodProbeAck.Rejected(RejectedReason.DeviceBusy);
                }
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

            MqttPublishAck puback = await _connection.PublishAsync(probeAckPublish);

            if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
            {
                Trace.TraceError("Failed to send the response to a direct method probe because the MQTT broker rejected the publish with reason code {0} and reason string {1}", puback.ReasonCode, puback.ReasonString);

                // The service never got this ready token, so don't hold capacity for it.
                if (readyToken != null)
                {
                    TryReleaseReadyToken(readyToken);
                }
            }
        }

        // ready_wait_budget = remaining connect budget + response timeout + safety margin, capped at the longest due time a timer supports.
        private static TimeSpan ReadyWaitBudget(uint remainingConnectTimeoutInSeconds, uint responseTimeoutSeconds)
        {
            double seconds = (double)remainingConnectTimeoutInSeconds + responseTimeoutSeconds + SafetyMarginSeconds(responseTimeoutSeconds);
            return TimeSpan.FromMilliseconds(Math.Min(seconds * 1000, MaxTimerDueTimeMilliseconds));
        }

        private static void ArmReadyWaitTimer(ReadyToken readyToken, TimeSpan readyWait)
        {
            try
            {
                readyToken.ReadyWaitTimer!.Change(readyWait, Timeout.InfiniteTimeSpan);
            }
            catch (ObjectDisposedException)
            {
                // An exec already claimed the token and disposed its timer, so there is nothing left to expire.
            }
        }

        private bool TryReserveCapacity()
        {
            while (true)
            {
                int active = Volatile.Read(ref _activeDirectMethods);
                if (active >= _maxConcurrentDirectMethods)
                {
                    return false;
                }

                if (Interlocked.CompareExchange(ref _activeDirectMethods, active + 1, active) == active)
                {
                    return true;
                }
            }
        }

        private void ReleaseCapacity()
        {
            Interlocked.Decrement(ref _activeDirectMethods);
        }

        // Releases the token's capacity slot unless an exec or the ready wait timer already claimed it.
        private bool TryReleaseReadyToken(ReadyToken readyToken)
        {
            if (!_readyTokens.TryRemove(new KeyValuePair<Guid, ReadyToken>(readyToken.RequestId, readyToken)))
            {
                return false;
            }

            readyToken.Dispose();
            ReleaseCapacity();
            return true;
        }

        private void OnReadyWaitExpired(object? state)
        {
            ReadyToken readyToken = (ReadyToken)state!;
            if (TryReleaseReadyToken(readyToken))
            {
                Trace.TraceWarning(
                    "No direct method exec message arrived for method {0} (request Id {1}) within its ready wait budget, so this client abandoned it.",
                    readyToken.MethodName,
                    readyToken.RequestId);
            }
        }

        private static uint SafetyMarginSeconds(uint responseTimeoutSeconds)
        {
            return Math.Clamp(responseTimeoutSeconds / 10, 1u, 10u);
        }

        private void ReleaseAllReadyTokens()
        {
            foreach (ReadyToken readyToken in _readyTokens.Values)
            {
                TryReleaseReadyToken(readyToken);
            }
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            ReleaseAllReadyTokens();
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
            ReleaseAllReadyTokens();
            _connection.Dispose();
        }

        // A probe this client accepted, awaiting its exec. Holds one capacity slot while it is in _readyTokens.
        private sealed class ReadyToken : IDisposable
        {
            internal ReadyToken(Guid requestId, ByteString readyId, string methodName)
            {
                RequestId = requestId;
                ReadyId = readyId;
                MethodName = methodName;
            }

            internal Guid RequestId { get; }

            internal ByteString ReadyId { get; }

            internal string MethodName { get; }

            internal ITimer? ReadyWaitTimer { get; set; }

            public void Dispose()
            {
                ReadyWaitTimer?.Dispose();
            }
        }
    }
}
