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
    /// <remarks>
    /// Each invocation runs through the direct method state machine: a probe that this client accepts puts the invocation
    /// in the ready state, the matching exec message moves it to executing, and the method handler's return completes it.
    /// The state machine is bound only by its own timers, not by the MQTT connection, so an invocation survives disconnects
    /// and reconnects.
    /// </remarks>
    public class DirectMethodClient : IDisposable
    {
        private const string ProtobufContentType = "application/protobuf";
        private const int MaxResponsePayloadSizeInBytes = 128 * 1024;

        // The connection client substitutes this device's current ID for the placeholder when it sends each response, so a
        // response still goes to the right topic if the device ID changes (for example, after re-provisioning).
        private const string DirectMethodsResponseTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/methods";
        private const string DirectMethodsRequestTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/dev/methods";

        private static readonly TimeSpan MinimumDefaultSafetyMargin = TimeSpan.FromSeconds(1);
        private static readonly TimeSpan MaximumDefaultSafetyMargin = TimeSpan.FromSeconds(10);

        // The longest due time System.Threading.Timer accepts.
        private const double MaxTimerDueTimeMilliseconds = uint.MaxValue - 1.0;

        // Every invocation this client accepted a probe for, keyed by request Id, from the moment it enters ready until its ready
        // wait deadline has passed and its method handler (if any) has returned. Keeping finished invocations until that deadline
        // lets a redelivered probe for them be recognized as a duplicate rather than accepted again.
        private readonly ConcurrentDictionary<Guid, Invocation> _invocations = new();

        // Invocations in ready plus executing method handlers. Never exceeds _maxConcurrentDirectMethods.
        private int _activeDirectMethods;
        private readonly int _maxConcurrentDirectMethods;
        private readonly TimeSpan? _safetyMargin;
        private readonly TimeProvider _timeProvider;

        // Request Ids of probes currently being answered, so only one delivery of a request is admitted at a time.
        private readonly ConcurrentDictionary<Guid, byte> _probesInProgress = new();

        // Probe handling, method handlers and abandon publishes run here rather than on the connection's receive path, so a
        // slow handler or a publish that waits out a reconnect never holds up other inbound messages.
        private readonly ConcurrentDictionary<Task, byte> _backgroundWork = new();
        private readonly CancellationTokenSource _disposalCancellation = new();

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
        /// <para>
        /// Only one handler can be added at a time because only one direct method response can be sent per request.
        /// </para>
        /// <para>
        /// The handler runs in the background, so up to <see cref="DirectMethodClientOptions.MaxConcurrentDirectMethods"/> invocations can run at
        /// once without holding up other messages from IoT hub.
        /// </para>
        /// <para>
        /// The handler must finish before <see cref="DirectMethodRequestReceivedEventArgs.CancellationToken"/> is cancelled, which happens at the
        /// remaining response timeout minus the safety margin (see <see cref="DirectMethodClientOptions.SafetyMargin"/>). A response returned after
        /// that is discarded. A handler that ignores the token keeps its invocation counted against
        /// <see cref="DirectMethodClientOptions.MaxConcurrentDirectMethods"/> until it returns.
        /// </para>
        /// <para>
        /// Response payloads must not exceed 128 KiB. An oversized response is logged and not published.
        /// </para>
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
        /// <para>
        /// This feature is only supported by IoT hubs that use MQTTv5. Older IoT hubs will never send this probe.
        /// Only one handler can be added at a time because only one probe response can be sent per request.
        /// </para>
        /// <para>
        /// Return <see cref="DirectMethodProbeAck.Accepted(TimeSpan)"/> to declare the least time the method needs to run. This client rejects the
        /// probe with <see cref="RejectedReason.InsufficientTime"/> if the response timeout, less the safety margin, can't cover that time, and
        /// refuses to start the method (abandoning it instead) if too little of the response timeout is left when the exec message arrives.
        /// </para>
        /// <para>
        /// If the handler accepts a probe while this client already holds <see cref="DirectMethodClientOptions.MaxConcurrentDirectMethods"/>
        /// invocations, this client sends a <see cref="RejectedReason.DeviceBusy"/> rejection instead.
        /// </para>
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
        /// <exception cref="ArgumentOutOfRangeException">
        /// Thrown if <see cref="DirectMethodClientOptions.MaxConcurrentDirectMethods"/> is less than 1, or <see cref="DirectMethodClientOptions.SafetyMargin"/> is negative.
        /// </exception>
        public DirectMethodClient(IConnectionClient connection, DirectMethodClientOptions options)
            : this(connection, options, TimeProvider.System)
        {
        }

        internal DirectMethodClient(IConnectionClient connection, DirectMethodClientOptions options, TimeProvider timeProvider)
        {
            ArgumentNullException.ThrowIfNull(options);
            ArgumentOutOfRangeException.ThrowIfLessThan(options.MaxConcurrentDirectMethods, 1, $"{nameof(options)}.{nameof(options.MaxConcurrentDirectMethods)}");
            if (options.SafetyMargin.HasValue)
            {
                ArgumentOutOfRangeException.ThrowIfLessThan(options.SafetyMargin.Value, TimeSpan.Zero, $"{nameof(options)}.{nameof(options.SafetyMargin)}");
            }

            _maxConcurrentDirectMethods = options.MaxConcurrentDirectMethods;
            _safetyMargin = options.SafetyMargin;
            _timeProvider = timeProvider;
            _connection = connection;
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
        }

        // Completes once every probe, method handler and publish this client started in the background has finished.
        internal async Task WhenIdleAsync()
        {
            while (!_backgroundWork.IsEmpty)
            {
                try
                {
                    await Task.WhenAll(_backgroundWork.Keys);
                }
                catch (Exception)
                {
                    // Background work logs its own failures.
                }
            }
        }

        private Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (!args.Publish.Topic.StartsWith("ih/") || !args.Publish.Topic.EndsWith("/dev/methods"))
            {
                // The publish is not relevant to this client, so ignore it. This check needs to happen prior to checking the deviceId within the topic b/c deviceId is
                // not available until after provisioning finishes and this client may be setup prior to provisioning. This allows this client to ignore DPS
                // publishes without needing to know the deviceId.
                return Task.CompletedTask;
            }

            var currentConnectionContext = _connection.GetCurrentConnectionContext();

            if (currentConnectionContext == null)
            {
                // This should never happen barring some race condition?
                Trace.TraceWarning("Received a direct method request, but the connection was lost. Ignoring it.");
                return Task.CompletedTask;
            }

            if (!args.Publish.Topic.Equals(DirectMethodsRequestTopic.Replace(IConnectionClient.DeviceIdPlaceholder, currentConnectionContext.DeviceId, StringComparison.Ordinal)))
            {
                // Message isn't relevant to this device. Ignore it.
                return Task.CompletedTask;
            }

            MqttPublish publish = args.Publish;
            if (!publish.UserProperties.TryGetType(out string? directMethodMessageType, out int? directMethodMessageTypeValue))
            {
                Trace.TraceWarning("Received a direct method message, but it is either missing the message type or the message type is malformed. Ignoring it.");
                return Task.CompletedTask;
            }

            if (directMethodMessageTypeValue != 1)
            {
                // The service may increment the protocol version of these messages over time. For now, there is only the one version, though
                Trace.TraceWarning("Received a direct method message whose type version ({0}) is not supported by this client (supported version: {1}). You may need to upgrade this library's version to handle this kind of message. Ignoring it.", directMethodMessageTypeValue, 1);
                return Task.CompletedTask;
            }

            if (directMethodMessageType.Equals("probe", StringComparison.Ordinal))
            {
                HandleProbe(publish);
            }
            else if (directMethodMessageType.Equals("exec", StringComparison.Ordinal))
            {
                HandleExec(publish);
            }
            else
            {
                Trace.TraceWarning("Received a direct method message of unrecognized type {0}. Ignoring it.", directMethodMessageType);
            }

            return Task.CompletedTask;
        }

        private void HandleProbe(MqttPublish publish)
        {
            // The connect budget is measured from the moment the probe arrived, before any handler runs.
            long probeReceivedTimestamp = _timeProvider.GetTimestamp();

            if (!GuidExtensions.TryParseBytes(publish.CorrelationData, out Guid? requestId))
            {
                Trace.TraceWarning("Received a direct method probe message with a malformed request Id. Ignoring it.");
                return;
            }

            // Only one delivery of a request is admitted at a time, so overlapping redeliveries can't send conflicting probe acks.
            if (!_probesInProgress.TryAdd(requestId.Value, 0))
            {
                // A redelivery of a probe this client is still answering.
                return;
            }

            bool handedOff = false;
            try
            {
                // Checked only while holding the gate, so the lookup can't be stale relative to a delivery that just accepted it.
                if (_invocations.ContainsKey(requestId.Value))
                {
                    // A redelivery of a probe this client already accepted. The original probe ack has its own QoS 1 delivery lifecycle.
                    Trace.TraceInformation("Received a duplicate direct method probe for request Id {0}. Ignoring it.", requestId.Value);
                    return;
                }

                Probe probe;
                try
                {
                    probe = Probe.Parser.ParseFrom(publish.Payload);
                }
                catch (InvalidProtocolBufferException ex)
                {
                    Trace.TraceError("Received direct method probe message for request Id {0} with a malformed payload. Ignoring it. Error: {1}", requestId.Value, ex.Message);
                    return;
                }

                Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? methodHandler = _directMethodInvoked.Handler;
                Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>>? probeHandler = _directMethodProbeReceived.Handler;

                Guid probeRequestId = requestId.Value;
                RunInBackground(async () =>
                {
                    try
                    {
                        await HandleProbeAsync(publish, probeRequestId, probe, probeReceivedTimestamp, methodHandler, probeHandler);
                    }
                    finally
                    {
                        _probesInProgress.TryRemove(probeRequestId, out _);
                    }
                });
                handedOff = true;
            }
            finally
            {
                if (!handedOff)
                {
                    _probesInProgress.TryRemove(requestId.Value, out _);
                }
            }
        }

        private async Task HandleProbeAsync(
            MqttPublish publish,
            Guid requestId,
            Probe probe,
            long probeReceivedTimestamp,
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? methodHandler,
            Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>>? probeHandler)
        {
            DirectMethodProbeAck? probeAck;
            if (methodHandler == null || probeHandler == null)
            {
                // Without both callbacks this client can't execute the method, so tell the service rather than letting the probe time out.
                Trace.TraceError("Received a direct method probe, but this client has not registered both callbacks, so it is rejecting the probe as method not found.");
                probeAck = DirectMethodProbeAck.Rejected(RejectedReason.MethodNotFound);
            }
            else
            {
                try
                {
                    probeAck = await probeHandler.Invoke(new() { MethodName = probe.MethodName, ResponseTimeoutSeconds = probe.ResponseTimeoutSeconds });
                }
                catch (Exception e)
                {
                    Trace.TraceError("The direct method probe handler threw for method {0} (request Id {1}), so no probe ack will be sent. {2}", probe.MethodName, requestId, e);
                    return;
                }

                if (probeAck == null)
                {
                    Trace.TraceError("The direct method probe handler returned null for method {0} (request Id {1}), so no probe ack will be sent.", probe.MethodName, requestId);
                    return;
                }
            }

            // The broker reduced the probe's message expiry by the time it sat queued, so it is the connect budget still left on arrival.
            TimeSpan remainingConnectBudget = TimeSpan.FromSeconds(publish.MessageExpiryInterval) - _timeProvider.GetElapsedTime(probeReceivedTimestamp);

            if (remainingConnectBudget <= TimeSpan.Zero)
            {
                Trace.TraceWarning("Application did not respond to direct method probe message before the connect timeout elapsed, so no probe ack message will be sent.");
                return;
            }

            // Round up so a fraction of a second left still yields a nonzero expiry; the exact budget bounds the publish attempt itself.
            uint remainingConnectTimeoutInSeconds = (uint)Math.Min(Math.Ceiling(remainingConnectBudget.TotalSeconds), uint.MaxValue);

            Invocation? invocation = null;
            if (probeAck.ProbeAck.Ready != null)
            {
                TimeSpan safetyMargin = SafetyMargin(probe.ResponseTimeoutSeconds);
                TimeSpan bestCaseExecutionBudget = TimeSpan.FromSeconds(probe.ResponseTimeoutSeconds) - safetyMargin;

                if (bestCaseExecutionBudget <= probeAck.MinimumExecutionTime)
                {
                    Trace.TraceWarning(
                        "Rejecting a direct method probe for method {0} as insufficient time: its response timeout of {1} s less the {2} safety margin can't cover its minimum execution time of {3}.",
                        probe.MethodName,
                        probe.ResponseTimeoutSeconds,
                        safetyMargin,
                        probeAck.MinimumExecutionTime);
                    probeAck = DirectMethodProbeAck.Rejected(RejectedReason.InsufficientTime);
                }
                else if (TryReserveCapacity())
                {
                    invocation = new Invocation(
                        requestId,
                        probeAck.ProbeAck.Ready.ReadyId,
                        probe.MethodName,
                        probe.ResponseTimeoutSeconds,
                        safetyMargin,
                        probeAck.MinimumExecutionTime,
                        publish.CorrelationData);

                    // Create the timer disarmed and arm it only once the invocation is registered, so it can't fire before there is an
                    // invocation for it to abandon.
                    invocation.ReadyWaitTimer = _timeProvider.CreateTimer(OnReadyWaitExpired, invocation, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);

                    if (!_invocations.TryAdd(requestId, invocation))
                    {
                        // Unreachable while _probesInProgress admits one delivery per request, but never leak the slot.
                        invocation.Dispose();
                        ReleaseCapacity();
                        return;
                    }

                    // Disposal may have drained _invocations while the probe callback ran. It cancels before draining, so either it saw
                    // this invocation or this check sees the cancellation; either way no timer is left holding on to a disposed client.
                    if (_disposalCancellation.IsCancellationRequested)
                    {
                        DiscardUnannouncedInvocation(invocation);
                        return;
                    }

                    // ready_wait_budget = remaining connect budget + response timeout + safety margin, measured from when this
                    // invocation entered ready.
                    ArmReadyWaitTimer(invocation, remainingConnectBudget + TimeSpan.FromSeconds(probe.ResponseTimeoutSeconds) + safetyMargin);
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

            // A probe ack that can't be sent before the connect budget runs out is no use to the service, which has already given up.
            PublishOutcome outcome = await TryPublishAsync(probeAckPublish, remainingConnectBudget, "probe ack", requestId);

            // Only a broker rejection proves the service never got this ready token. If delivery is merely unconfirmed (for example, the
            // PUBACK was lost to a disconnect), the probe ack may still reach the service, so the ready state is kept until its ready wait
            // deadline in case the matching exec arrives.
            if (outcome == PublishOutcome.Rejected && invocation != null)
            {
                DiscardUnannouncedInvocation(invocation);
            }
        }

        private void HandleExec(MqttPublish publish)
        {
            // The exec budget is measured from the moment the exec arrived.
            long execReceivedTimestamp = _timeProvider.GetTimestamp();

            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? methodHandler = _directMethodInvoked.Handler;
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

            if (!_invocations.TryGetValue(requestId.Value, out Invocation? invocation))
            {
                Trace.TraceWarning("Received direct method exec message with unknown request Id. Ignoring it.");
                return;
            }

            Exec exec;
            try
            {
                exec = Exec.Parser.ParseFrom(publish.Payload);
            }
            catch (InvalidProtocolBufferException ex)
            {
                // The invocation stays ready, so a well-formed exec for the same request can still be handled.
                Trace.TraceError("Received direct method exec message for request Id {0} with a malformed payload. Ignoring it and continuing to wait for a valid exec message for this request. Error: {1}", requestId.Value, ex.Message);
                return;
            }

            // The broker reduced the exec's message expiry by the time it sat queued, so it is the response timeout still left on arrival.
            TimeSpan execBudget = TimeSpan.FromSeconds(publish.MessageExpiryInterval);

            RunInBackground(() => AdmitAndExecuteAsync(invocation, exec, methodHandler, execReceivedTimestamp, execBudget));
        }

        private async Task AdmitAndExecuteAsync(
            Invocation invocation,
            Exec exec,
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> methodHandler,
            long execReceivedTimestamp,
            TimeSpan execBudget)
        {
            TimeSpan effectiveBudget = execBudget - invocation.SafetyMargin;
            TimeSpan remainingEffectiveBudget;
            CancellationTokenSource? executionDeadline = null;

            // Exec admission and the ready wait timer both decide the fate of the same ready state, so they are serialized on it.
            bool notReady;
            bool readyIdMatches;
            bool admitted = false;
            lock (invocation)
            {
                notReady = invocation.State != InvocationState.Ready;

                // An exec without this ready state's token leaves the ready state for the exec that does carry it.
                readyIdMatches = invocation.ReadyId.Equals(exec.ReadyId);

                // Measured here, at the moment of admission, so time the exec spent waiting to be scheduled or for this lock counts against it.
                remainingEffectiveBudget = effectiveBudget - _timeProvider.GetElapsedTime(execReceivedTimestamp);

                if (!notReady && readyIdMatches)
                {
                    admitted = remainingEffectiveBudget > invocation.MinimumExecutionTime;
                    if (admitted)
                    {
                        // Started together with the transition, so the handler's deadline is exactly what admission allowed for.
                        executionDeadline = new CancellationTokenSource(remainingEffectiveBudget, _timeProvider);
                        invocation.State = InvocationState.Executing;
                    }
                    else
                    {
                        invocation.State = InvocationState.Abandoned;
                        ReleaseCapacityLocked(invocation);
                    }
                }
            }

            if (notReady)
            {
                Trace.TraceWarning("Received direct method exec message for request Id {0}, which is no longer awaiting one. Ignoring it.", invocation.RequestId);
                return;
            }

            if (!readyIdMatches)
            {
                Trace.TraceWarning("Received direct method exec message for request Id {0} with an unexpected ready Id. Ignoring it.", invocation.RequestId);
                return;
            }

            if (!admitted)
            {
                Trace.TraceWarning(
                    "The direct method exec message for method {0} (request Id {1}) left {2} after the safety margin, which is not more than its minimum execution time of {3}, so this client abandoned it without running it.",
                    invocation.MethodName,
                    invocation.RequestId,
                    remainingEffectiveBudget,
                    invocation.MinimumExecutionTime);
                PublishAbandonInBackground(invocation, AbandonReason.InsufficientTime);
                return;
            }

            using (executionDeadline)
            {
                await ExecuteAsync(invocation, exec, methodHandler, execReceivedTimestamp, execBudget, effectiveBudget, executionDeadline!);
            }
        }

        // The execution deadline leaves the safety margin for the result to travel back before the service's response timer fires.
        private async Task ExecuteAsync(
            Invocation invocation,
            Exec exec,
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> methodHandler,
            long execReceivedTimestamp,
            TimeSpan execBudget,
            TimeSpan effectiveBudget,
            CancellationTokenSource executionDeadline)
        {

            DirectMethodRequestReceivedEventArgs directMethodInvokedArgs = new()
            {
                MethodName = invocation.MethodName,
                Payload = exec.Params.Span.ToArray(),
                CancellationToken = executionDeadline.Token
            };

            DirectMethodResponse? methodResponse;
            try
            {
                methodResponse = await methodHandler.Invoke(directMethodInvokedArgs);
            }
            catch (OperationCanceledException) when (executionDeadline.IsCancellationRequested)
            {
                Trace.TraceWarning("The direct method with name {0} did not finish before its execution deadline, so no result message will be sent.", invocation.MethodName);
                return;
            }
            catch (Exception e)
            {
                Trace.TraceError("The direct method handler for method {0} (request Id {1}) threw, so no result message will be sent. {2}", invocation.MethodName, invocation.RequestId, e);
                return;
            }
            finally
            {
                // The invocation stops counting against the capacity limit once the handler returns control, even if it ran past its deadline.
                CompleteExecution(invocation);
            }

            if (methodResponse == null)
            {
                // This is a special case that is mostly reserved for the unified direct method client to communicate back to this client that no handler was registered, so no response should be sent by this client.
                return;
            }

            TimeSpan elapsedSinceExec = _timeProvider.GetElapsedTime(execReceivedTimestamp);
            if (executionDeadline.IsCancellationRequested || elapsedSinceExec >= effectiveBudget)
            {
                // The invocation timed out locally, so its token is terminal and no result may be published for it.
                Trace.TraceWarning("Application did not respond to the direct method {0} before its execution deadline, so no result message will be sent.", invocation.MethodName);
                return;
            }

            if (methodResponse.Payload != null && methodResponse.Payload.Length > MaxResponsePayloadSizeInBytes)
            {
                Trace.TraceError("The response to direct method {0} (request Id {1}) has a payload of {2} bytes, which exceeds the 128 KiB limit, so it will not be sent.", invocation.MethodName, invocation.RequestId, methodResponse.Payload.Length);
                return;
            }

            Result result = new()
            {
                Status = methodResponse.Status,
                Body = methodResponse.Payload is null
                    ? ByteString.Empty
                    : ByteString.CopyFrom(methodResponse.Payload)
            };

            // The result keeps the full remaining exec budget, with no safety margin, so it has every chance to reach the service in time.
            TimeSpan remainingExecBudget = execBudget - elapsedSinceExec;
            MqttPublish resultPublish = new()
            {
                Topic = DirectMethodsResponseTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                Payload = result.ToByteArray(),
                CorrelationData = invocation.CorrelationData,
                MessageExpiryInterval = (uint)Math.Ceiling(remainingExecBudget.TotalSeconds),
                ContentType = ProtobufContentType
            };

            resultPublish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("result:1")));

            await TryPublishAsync(resultPublish, remainingExecBudget, "result", invocation.RequestId);
        }

        private void PublishAbandonInBackground(Invocation invocation, AbandonReason reason)
        {
            Abandon abandon = new()
            {
                ReadyId = invocation.ReadyId,
                Reason = reason,
            };

            // A late abandon is harmless (the service ignores it once the invocation is over), so it may live for the whole response timeout.
            uint expiry = Math.Max(invocation.ResponseTimeoutSeconds, 1u);
            MqttPublish abandonPublish = new()
            {
                Topic = DirectMethodsResponseTopic,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                Payload = abandon.ToByteArray(),
                CorrelationData = invocation.CorrelationData,
                MessageExpiryInterval = expiry,
                ContentType = ProtobufContentType
            };

            abandonPublish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("abandon:1")));

            RunInBackground(() => TryPublishAsync(abandonPublish, TimeSpan.FromSeconds(expiry), "abandon", invocation.RequestId));
        }

        // Publishes, giving up once the message would have expired or this client is disposed.
        private async Task<PublishOutcome> TryPublishAsync(MqttPublish publish, TimeSpan expiresAfter, string description, Guid requestId)
        {
            using CancellationTokenSource expiry = CancellationTokenSource.CreateLinkedTokenSource(_disposalCancellation.Token);
            expiry.CancelAfter(TimeSpan.FromMilliseconds(Math.Clamp(expiresAfter.TotalMilliseconds, 0, MaxTimerDueTimeMilliseconds)));

            try
            {
                MqttPublishAck puback = await _connection.PublishAsync(publish, expiry.Token);
                if (puback.ReasonCode != MqttPublishAckReasonCode.Success)
                {
                    Trace.TraceError(
                        "Failed to send the direct method {0} for request Id {1} because the MQTT broker rejected the publish with reason code {2} and reason string {3}",
                        description,
                        requestId,
                        puback.ReasonCode,
                        puback.ReasonString);
                    return PublishOutcome.Rejected;
                }

                return PublishOutcome.Accepted;
            }
            catch (OperationCanceledException) when (expiry.IsCancellationRequested)
            {
                Trace.TraceWarning("Gave up waiting for the direct method {0} for request Id {1} to be acknowledged because it expired, or this client was disposed. It may still have been delivered.", description, requestId);
                return PublishOutcome.Unconfirmed;
            }
            catch (Exception e)
            {
                Trace.TraceError("Failed to confirm delivery of the direct method {0} for request Id {1}. It may still have been delivered. {2}", description, requestId, e);
                return PublishOutcome.Unconfirmed;
            }
        }
        private void RunInBackground(Func<Task> work)
        {
            Task task = Task.Run(work);
            _backgroundWork.TryAdd(task, 0);
            _ = task.ContinueWith(
                completed =>
                {
                    _backgroundWork.TryRemove(completed, out _);
                    if (completed.IsFaulted)
                    {
                        Trace.TraceError("Direct method background work failed. {0}", completed.Exception);
                    }
                },
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }

        private TimeSpan SafetyMargin(uint responseTimeoutSeconds)
        {
            // safety_margin = clamp(response_timeout_seconds * 0.10, 1s, 10s) unless the application configured its own.
            return _safetyMargin
                ?? TimeSpan.FromSeconds(Math.Clamp(responseTimeoutSeconds * 0.1, MinimumDefaultSafetyMargin.TotalSeconds, MaximumDefaultSafetyMargin.TotalSeconds));
        }

        private static void ArmReadyWaitTimer(Invocation invocation, TimeSpan readyWait)
        {
            TimeSpan dueTime = TimeSpan.FromMilliseconds(Math.Clamp(readyWait.TotalMilliseconds, 0, MaxTimerDueTimeMilliseconds));
            try
            {
                invocation.ReadyWaitTimer!.Change(dueTime, Timeout.InfiniteTimeSpan);
            }
            catch (ObjectDisposedException)
            {
                // The invocation was already discarded, so there is nothing left to expire.
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

        // Must be called while holding the invocation's lock.
        private void ReleaseCapacityLocked(Invocation invocation)
        {
            if (invocation.HoldsCapacity)
            {
                invocation.HoldsCapacity = false;
                ReleaseCapacity();
            }
        }

        // Removes the invocation from tracking, if it is still the one tracked for its request Id.
        private void Forget(Invocation invocation)
        {
            if (_invocations.TryRemove(new KeyValuePair<Guid, Invocation>(invocation.RequestId, invocation)))
            {
                invocation.Dispose();
            }
        }

        private void OnReadyWaitExpired(object? state)
        {
            Invocation invocation = (Invocation)state!;
            bool abandoned = false;
            bool forget;
            lock (invocation)
            {
                invocation.ReadyWaitElapsed = true;
                if (invocation.State == InvocationState.Ready)
                {
                    // Committed before the abandon is published, so publishing it asserts that execution never began.
                    invocation.State = InvocationState.Abandoned;
                    ReleaseCapacityLocked(invocation);
                    abandoned = true;
                }

                // An executing invocation is forgotten when its handler returns instead.
                forget = invocation.State != InvocationState.Executing;
            }

            if (forget)
            {
                Forget(invocation);
            }

            if (abandoned)
            {
                Trace.TraceWarning(
                    "No direct method exec message arrived for method {0} (request Id {1}) within its ready wait budget, so this client abandoned it.",
                    invocation.MethodName,
                    invocation.RequestId);
                PublishAbandonInBackground(invocation, AbandonReason.ReadyWaitTimeout);
            }
        }

        private void CompleteExecution(Invocation invocation)
        {
            bool forget;
            lock (invocation)
            {
                invocation.State = InvocationState.Completed;
                ReleaseCapacityLocked(invocation);
                forget = invocation.ReadyWaitElapsed;
            }

            if (forget)
            {
                Forget(invocation);
            }
        }

        // For an invocation whose ready token the service never received: the broker rejected its probe ack, or the client was disposed
        // before the probe ack was sent.
        private void DiscardUnannouncedInvocation(Invocation invocation)
        {
            lock (invocation)
            {
                if (invocation.State != InvocationState.Ready)
                {
                    // The probe ack reached the service after all (its exec arrived) or the ready wait already ended it.
                    return;
                }

                invocation.State = InvocationState.Abandoned;
                ReleaseCapacityLocked(invocation);
            }

            Forget(invocation);
        }

        private void ReleaseAllInvocations()
        {
            foreach (Invocation invocation in _invocations.Values)
            {
                lock (invocation)
                {
                    if (invocation.State == InvocationState.Ready)
                    {
                        invocation.State = InvocationState.Abandoned;
                        ReleaseCapacityLocked(invocation);
                    }
                }

                Forget(invocation);
            }
        }

        private void DisposeCore()
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            try
            {
                _disposalCancellation.Cancel();
            }
            catch (ObjectDisposedException)
            {
            }

            ReleaseAllInvocations();
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            DisposeCore();
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
            DisposeCore();
            _connection.Dispose();
        }

        private enum PublishOutcome
        {
            // The broker acknowledged the publish.
            Accepted,

            // The broker refused the publish, so it was not delivered.
            Rejected,

            // No acknowledgement arrived (for example, it was lost to a disconnect), so the publish may or may not have been delivered.
            Unconfirmed,
        }

        private enum InvocationState
        {
            // Accepted the probe and waiting for the matching exec.
            Ready,

            // The method handler is running.
            Executing,

            // Terminal: the ready state ended before execution began.
            Abandoned,

            // Terminal: the method handler returned (whether or not a result was published).
            Completed,
        }

        // One direct method invocation's device-side state machine. Guarded by locking on the instance.
        private sealed class Invocation : IDisposable
        {
            internal Invocation(
                Guid requestId,
                ByteString readyId,
                string methodName,
                uint responseTimeoutSeconds,
                TimeSpan safetyMargin,
                TimeSpan minimumExecutionTime,
                byte[]? correlationData)
            {
                RequestId = requestId;
                ReadyId = readyId;
                MethodName = methodName;
                ResponseTimeoutSeconds = responseTimeoutSeconds;
                SafetyMargin = safetyMargin;
                MinimumExecutionTime = minimumExecutionTime;
                CorrelationData = correlationData;
            }

            internal Guid RequestId { get; }

            // Generated once on entering ready and never changed afterwards.
            internal ByteString ReadyId { get; }

            internal string MethodName { get; }

            internal uint ResponseTimeoutSeconds { get; }

            internal TimeSpan SafetyMargin { get; }

            internal TimeSpan MinimumExecutionTime { get; }

            internal byte[]? CorrelationData { get; }

            internal InvocationState State { get; set; } = InvocationState.Ready;

            // Whether this invocation holds one of the client's capacity slots. True from ready until it is abandoned or its handler returns.
            internal bool HoldsCapacity { get; set; } = true;

            // Whether the ready wait deadline has passed, after which this invocation is no longer needed to recognize a duplicate probe.
            internal bool ReadyWaitElapsed { get; set; }

            internal ITimer? ReadyWaitTimer { get; set; }

            public void Dispose()
            {
                ReadyWaitTimer?.Dispose();
            }
        }
    }
}
