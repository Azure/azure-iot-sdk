using Google.Protobuf;
using Google.Protobuf.WellKnownTypes;
using MQTTnet;
using MQTTnet.Packets;
using MQTTnet.Protocol;
using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using MethodsProto = Microsoft.Azure.Devices.Client.Models.DirectMethods;
using PresenceProto = Microsoft.Azure.Devices.Client.Gen2.Connection;
using TwinProto = Microsoft.Azure.Devices.Client.Models.Twin;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The gen2 (Azure Event Grid MQTT broker) half of the stub IoT hub service.
    /// </summary>
    /// <remarks>
    /// Modeled on the presence, twin and direct method design documents. All gen2 traffic uses the
    /// <c>ih/{deviceId}/{dir}/{feature}</c> topic space, a <c>type</c> user property of the form <c>{name}:{schemaVersion}</c> for
    /// handler dispatch, MQTT 5 correlation data for correlation, and protobuf payloads declared under <c>common/Protos</c>.
    /// </remarks>
    public sealed partial class StubIotHubService
    {
        private readonly ConcurrentDictionary<Guid, PendingGen2MethodInvocation> _pendingGen2MethodInvocations = new();

        private static string Gen2DeviceBoundTopic(string deviceId, string feature) => $"ih/{deviceId}/dev/{feature}";

        /// <summary>
        /// Publish a full-state twin push to a device outside of the birth handshake. This is the backend-initiated recovery
        /// mechanism described in the twin design doc section 4.10.
        /// </summary>
        /// <param name="deviceId">The device to push to.</param>
        /// <param name="includeDesired">Whether the push carries the desired section.</param>
        /// <param name="includeReported">Whether the push carries the reported section.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        public async Task PushTwinAsync(
            string deviceId,
            bool includeDesired = true,
            bool includeReported = false,
            CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentException.ThrowIfNullOrEmpty(deviceId);
            EnsureStarted();

            if (_options.Generation != IotHubGeneration.Gen2)
            {
                throw new NotSupportedException("Only a gen2 IoT hub pushes twin state to its devices.");
            }

            if (!includeDesired && !includeReported)
            {
                throw new ArgumentException("A twin push must carry at least one section.", nameof(includeDesired));
            }

            StubDeviceState device = GetDeviceState(deviceId);
            await PublishGen2TwinPushAsync(device, includeDesired, includeReported, cancellationToken);
        }

        private async Task HandleGen2MessageAsync(MqttApplicationMessageReceivedEventArgs args)
        {
            MqttApplicationMessage message = args.ApplicationMessage;

            // ih/{deviceId}/srv/{feature}
            string[] topicSegments = message.Topic.Split('/');
            if (topicSegments.Length != 4
                || !topicSegments[0].Equals("ih", StringComparison.Ordinal)
                || !topicSegments[2].Equals("srv", StringComparison.Ordinal))
            {
                return;
            }

            string deviceId = topicSegments[1];
            string feature = topicSegments[3];

            if (!IsServedDevice(deviceId))
            {
                return;
            }

            if (feature.Equals("telemetry", StringComparison.Ordinal))
            {
                HandleGen2Telemetry(deviceId, message);
                return;
            }

            if (!TryGetMessageType(message, out string? messageType, out int schemaVersion))
            {
                Log($"Discarding a gen2 message on '{message.Topic}' because its 'type' user property is missing or malformed.");
                return;
            }

            if (schemaVersion != 1)
            {
                Log($"Discarding a gen2 '{messageType}' message on '{message.Topic}' because schema version {schemaVersion} is not supported.");
                return;
            }

            switch (feature)
            {
                case "presence":
                    await HandleGen2PresenceAsync(deviceId, messageType, message);
                    break;

                case "twin":
                    await HandleGen2TwinAsync(deviceId, messageType, message);
                    break;

                case "methods":
                    HandleGen2Methods(deviceId, messageType, message);
                    break;

                default:
                    Log($"Discarding a gen2 message on unrecognized feature topic '{message.Topic}'.");
                    break;
            }
        }

        private void HandleGen2Telemetry(string deviceId, MqttApplicationMessage message)
        {
            // Gen2 telemetry uses native MQTT 5 message properties rather than the classic property-bag topic string.
            var userProperties = new Dictionary<string, string>(StringComparer.Ordinal);
            string? messageId = null;
            string? contentEncoding = null;

            if (message.UserProperties != null)
            {
                foreach (MqttUserProperty userProperty in message.UserProperties)
                {
                    switch (userProperty.Name)
                    {
                        case "$.mid":
                            messageId = userProperty.ReadValueAsString();
                            break;

                        case "$.ce":
                            contentEncoding = userProperty.ReadValueAsString();
                            break;

                        default:
                            userProperties[userProperty.Name] = userProperty.ReadValueAsString();
                            break;
                    }
                }
            }

            TelemetryReceived?.Invoke(this, new StubTelemetryReceivedEventArgs
            {
                DeviceId = deviceId,
                Payload = message.Payload.ToArray(),
                MessageId = messageId,
                CorrelationId = message.CorrelationData == null ? null : Encoding.UTF8.GetString(message.CorrelationData),
                ContentType = message.ContentType,
                ContentEncoding = contentEncoding,
                UserProperties = userProperties,
            });
        }

        private async Task HandleGen2PresenceAsync(string deviceId, string messageType, MqttApplicationMessage message)
        {
            if (!messageType.Equals("birth", StringComparison.Ordinal))
            {
                return;
            }

            if (!TryGetCorrelationGuid(message, out Guid connectionNonce))
            {
                Log($"Discarding a birth from '{deviceId}' because its correlation data is not a 16-byte connection nonce.");
                return;
            }

            PresenceProto.Birth birth = PresenceProto.Birth.Parser.ParseFrom(message.Payload);
            StubDeviceState device = GetDeviceState(deviceId);

            device.ConnectionNonce = connectionNonce;
            device.PushDesired = birth.PushDesired;
            device.PushReported = birth.PushReported;
            device.IsDispatchReady = true;

            StubTwinSnapshot twin = device.GetTwinSnapshot();

            // Birth-ack must be published before any birth-triggered twin push (twin design doc section 4.1).
            var birthAck = new PresenceProto.BirthAck
            {
                DesiredVersion = twin.DesiredVersion,
                ReportedVersion = twin.ReportedVersion,
            };

            await PublishAsync(
                BuildGen2Message(
                    Gen2DeviceBoundTopic(deviceId, "presence"),
                    "birth-ack:1",
                    birthAck.ToByteArray(),
                    connectionNonce.ToByteArray(bigEndian: true),
                    MqttQualityOfServiceLevel.AtMostOnce),
                CancellationToken.None);

            Log($"Admitted birth from '{deviceId}' (nonce {connectionNonce}), acked with desired v{twin.DesiredVersion} / reported v{twin.ReportedVersion}.");

            var birthArgs = new StubDeviceBirthEventArgs
            {
                DeviceId = deviceId,
                ConnectionNonce = connectionNonce,
                SessionPresent = birth.SessionPresent,
                DeviceDesiredVersion = birth.DesiredVersion,
                DeviceReportedVersion = birth.ReportedVersion,
                PushDesired = birth.PushDesired,
                PushReported = birth.PushReported,
            };

            // A section is pushed only if the device asked for it and its held version differs from the authoritative version
            // (twin design doc section 4.1).
            bool pushDesired = _options.EnableBirthTriggeredTwinPush && birth.PushDesired && birth.DesiredVersion != twin.DesiredVersion;
            bool pushReported = _options.EnableBirthTriggeredTwinPush && birth.PushReported && birth.ReportedVersion != twin.ReportedVersion;

            if (pushDesired || pushReported)
            {
                await PublishGen2TwinPushAsync(device, pushDesired, pushReported, CancellationToken.None);
            }

            GetBirthWaiter(deviceId).TrySetResult(birthArgs);
            DeviceBirthReceived?.Invoke(this, birthArgs);
        }

        private async Task HandleGen2TwinAsync(string deviceId, string messageType, MqttApplicationMessage message)
        {
            StubDeviceState device = GetDeviceState(deviceId);

            if (messageType.Equals("get", StringComparison.Ordinal))
            {
                TwinProto.TwinGet request = TwinProto.TwinGet.Parser.ParseFrom(message.Payload);
                StubTwinSnapshot twin = device.GetTwinSnapshot();

                bool wantsDesired = request.Sections is TwinProto.Sections.Desired or TwinProto.Sections.Both;
                bool wantsReported = request.Sections is TwinProto.Sections.Reported or TwinProto.Sections.Both;

                // Versions are always returned, even for a malformed SECTIONS_UNSPECIFIED request (twin design doc section 3.3).
                var response = new TwinProto.TwinGetResponse
                {
                    DesiredVersion = twin.DesiredVersion,
                    ReportedVersion = twin.ReportedVersion,
                };

                // A payload is omitted when the device's if-not-match filter already matches the authoritative version.
                if (wantsDesired && request.IfNotMatchDesired != twin.DesiredVersion)
                {
                    response.DesiredPayload = ByteString.CopyFromUtf8(twin.Desired.ToJsonString());
                }

                if (wantsReported && request.IfNotMatchReported != twin.ReportedVersion)
                {
                    response.ReportedPayload = ByteString.CopyFromUtf8(twin.Reported.ToJsonString());
                }

                await PublishAsync(
                    BuildGen2Message(
                        Gen2DeviceBoundTopic(deviceId, "twin"),
                        "get-response:1",
                        response.ToByteArray(),
                        message.CorrelationData,
                        MqttQualityOfServiceLevel.AtMostOnce),
                    CancellationToken.None);

                TwinGetReceived?.Invoke(this, new StubTwinGetReceivedEventArgs
                {
                    DeviceId = deviceId,
                    RequestedDesired = wantsDesired,
                    RequestedReported = wantsReported,
                });

                return;
            }

            if (messageType.Equals("reported-patch", StringComparison.Ordinal))
            {
                TwinProto.ReportedPatch request = TwinProto.ReportedPatch.Parser.ParseFrom(message.Payload);

                TwinProto.Result result;
                ulong version;
                JsonObject patch;

                try
                {
                    patch = JsonNode.Parse(request.Payload.ToStringUtf8())?.AsObject() ?? new JsonObject();
                }
                catch (JsonException)
                {
                    await PublishGen2ReportedPatchResponseAsync(deviceId, message.CorrelationData, TwinProto.Result.PayloadInvalid, device.GetTwinSnapshot().ReportedVersion);
                    return;
                }

                bool applied = device.TryApplyReportedPatch(patch, request.IfMatch, out version);
                result = applied ? TwinProto.Result.Ok : TwinProto.Result.VersionMismatch;

                await PublishGen2ReportedPatchResponseAsync(deviceId, message.CorrelationData, result, version);

                ReportedPropertiesReceived?.Invoke(this, new StubReportedPropertiesReceivedEventArgs
                {
                    DeviceId = deviceId,
                    Patch = patch,
                    IfMatch = request.IfMatch,
                    WasApplied = applied,
                    ReportedVersion = version,
                });

                return;
            }

            Log($"Discarding an unrecognized gen2 twin message of type '{messageType}' from '{deviceId}'.");
        }

        private Task PublishGen2ReportedPatchResponseAsync(string deviceId, byte[]? correlationData, TwinProto.Result result, ulong version)
        {
            var response = new TwinProto.ReportedPatchResponse
            {
                Result = result,
                Version = version,
            };

            return PublishAsync(
                BuildGen2Message(
                    Gen2DeviceBoundTopic(deviceId, "twin"),
                    "reported-patch-response:1",
                    response.ToByteArray(),
                    correlationData,
                    MqttQualityOfServiceLevel.AtMostOnce),
                CancellationToken.None);
        }

        private void HandleGen2Methods(string deviceId, string messageType, MqttApplicationMessage message)
        {
            if (!TryGetCorrelationGuid(message, out Guid requestId))
            {
                Log($"Discarding a gen2 '{messageType}' message from '{deviceId}' because its correlation data is not a 16-byte request id.");
                return;
            }

            if (!_pendingGen2MethodInvocations.TryGetValue(requestId, out PendingGen2MethodInvocation? pending))
            {
                // The invocation already reached a terminal state. A late probe-ack, result or abandon is simply ignored.
                return;
            }

            switch (messageType)
            {
                case "probe-ack":
                    pending.ProbeAckReceived.TrySetResult(MethodsProto.ProbeAck.Parser.ParseFrom(message.Payload));
                    break;

                case "result":
                    pending.ResultReceived.TrySetResult(MethodsProto.Result.Parser.ParseFrom(message.Payload));
                    break;

                case "abandon":
                    pending.Abandoned.TrySetResult(MethodsProto.Abandon.Parser.ParseFrom(message.Payload));
                    break;

                default:
                    Log($"Discarding an unrecognized gen2 direct method message of type '{messageType}' from '{deviceId}'.");
                    break;
            }
        }

        private async Task<StubDirectMethodResult> InvokeGen2DirectMethodAsync(
            string deviceId,
            string methodName,
            byte[]? payload,
            TimeSpan connectTimeout,
            TimeSpan responseTimeout,
            CancellationToken cancellationToken)
        {
            Guid requestId = Guid.NewGuid();
            byte[] correlationData = requestId.ToByteArray(bigEndian: true);
            var pending = new PendingGen2MethodInvocation();
            _pendingGen2MethodInvocations[requestId] = pending;

            try
            {
                var probe = new MethodsProto.Probe
                {
                    MethodName = methodName,
                    ResponseTimeoutSeconds = ToWholeSeconds(responseTimeout),
                };

                await PublishAsync(
                    BuildGen2Message(
                        Gen2DeviceBoundTopic(deviceId, "methods"),
                        "probe:1",
                        probe.ToByteArray(),
                        correlationData,
                        MqttQualityOfServiceLevel.AtLeastOnce,
                        ToWholeSeconds(connectTimeout)),
                    cancellationToken);

                Task<MethodsProto.ProbeAck> probeAckTask = pending.ProbeAckReceived.Task;
                Task<MethodsProto.Abandon> abandonTask = pending.Abandoned.Task;

                Task firstProbeOutcome;
                try
                {
                    firstProbeOutcome = await Task.WhenAny(probeAckTask, abandonTask).WaitAsync(connectTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    return new StubDirectMethodResult { Outcome = StubDirectMethodOutcome.TimedOut };
                }

                if (firstProbeOutcome == abandonTask)
                {
                    return BuildAbandonedResult(await abandonTask);
                }

                MethodsProto.ProbeAck probeAck = await probeAckTask;
                if (probeAck.ResultCase == MethodsProto.ProbeAck.ResultOneofCase.Rejected)
                {
                    return new StubDirectMethodResult
                    {
                        Outcome = StubDirectMethodOutcome.Rejected,
                        RejectedReason = probeAck.Rejected.Reason,
                    };
                }

                var exec = new MethodsProto.Exec
                {
                    ReadyId = probeAck.Ready.ReadyId,
                    Params = payload == null ? ByteString.Empty : ByteString.CopyFrom(payload),
                    ExecStart = Timestamp.FromDateTimeOffset(DateTimeOffset.UtcNow),
                };

                await PublishAsync(
                    BuildGen2Message(
                        Gen2DeviceBoundTopic(deviceId, "methods"),
                        "exec:1",
                        exec.ToByteArray(),
                        correlationData,
                        MqttQualityOfServiceLevel.AtLeastOnce,
                        ToWholeSeconds(responseTimeout)),
                    cancellationToken);

                Task<MethodsProto.Result> resultTask = pending.ResultReceived.Task;

                Task firstExecOutcome;
                try
                {
                    firstExecOutcome = await Task.WhenAny(resultTask, abandonTask).WaitAsync(responseTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    return new StubDirectMethodResult { Outcome = StubDirectMethodOutcome.TimedOut };
                }

                if (firstExecOutcome == abandonTask)
                {
                    return BuildAbandonedResult(await abandonTask);
                }

                MethodsProto.Result result = await resultTask;
                return new StubDirectMethodResult
                {
                    Outcome = StubDirectMethodOutcome.Completed,
                    Status = result.Status,
                    Payload = result.Body.ToByteArray(),
                };
            }
            finally
            {
                _pendingGen2MethodInvocations.TryRemove(requestId, out _);
            }
        }

        private static StubDirectMethodResult BuildAbandonedResult(MethodsProto.Abandon abandon)
        {
            return new StubDirectMethodResult
            {
                Outcome = StubDirectMethodOutcome.Abandoned,
                AbandonReason = abandon.Reason,
            };
        }

        private Task PublishGen2DesiredPatchAsync(StubDeviceState device, JsonObject patch, ulong newVersion, CancellationToken cancellationToken)
        {
            if (!device.IsDispatchReady || device.ConnectionNonce == null)
            {
                // A real hub only dispatches to a device whose presence is dispatch-ready. The version was still bumped, so the
                // device will pick the change up on its next birth-triggered push or GET.
                Log($"Skipping desired patch dispatch to '{device.DeviceId}' because it has not completed the presence handshake.");
                return Task.CompletedTask;
            }

            var desiredPatch = new TwinProto.DesiredPatch
            {
                Version = newVersion,
                Payload = ByteString.CopyFromUtf8(patch.ToJsonString()),
            };

            return PublishAsync(
                BuildGen2Message(
                    Gen2DeviceBoundTopic(device.DeviceId, "twin"),
                    "desired-patch:1",
                    desiredPatch.ToByteArray(),
                    device.ConnectionNonce.Value.ToByteArray(bigEndian: true),
                    MqttQualityOfServiceLevel.AtMostOnce),
                cancellationToken);
        }

        private Task PublishGen2TwinPushAsync(StubDeviceState device, bool includeDesired, bool includeReported, CancellationToken cancellationToken)
        {
            StubTwinSnapshot twin = device.GetTwinSnapshot();
            var twinPush = new TwinProto.TwinPush();

            if (includeDesired)
            {
                twinPush.Desired = new TwinProto.Section
                {
                    Version = twin.DesiredVersion,
                    Payload = ByteString.CopyFromUtf8(twin.Desired.ToJsonString()),
                };
            }

            if (includeReported)
            {
                twinPush.Reported = new TwinProto.Section
                {
                    Version = twin.ReportedVersion,
                    Payload = ByteString.CopyFromUtf8(twin.Reported.ToJsonString()),
                };
            }

            return PublishAsync(
                BuildGen2Message(
                    Gen2DeviceBoundTopic(device.DeviceId, "twin"),
                    "twin-push:1",
                    twinPush.ToByteArray(),
                    device.ConnectionNonce?.ToByteArray(bigEndian: true),
                    MqttQualityOfServiceLevel.AtMostOnce),
                cancellationToken);
        }

        private static MqttApplicationMessage BuildGen2Message(
            string topic,
            string type,
            byte[] payload,
            byte[]? correlationData,
            MqttQualityOfServiceLevel qualityOfService,
            uint messageExpiryIntervalSeconds = 0)
        {
            MqttApplicationMessageBuilder builder = new MqttApplicationMessageBuilder()
                .WithTopic(topic)
                .WithPayload(payload)
                .WithQualityOfServiceLevel(qualityOfService)
                .WithContentType(ProtobufContentType)
                .WithUserProperty("type", (ReadOnlyMemory<byte>)Encoding.UTF8.GetBytes(type));

            if (correlationData != null)
            {
                builder.WithCorrelationData(correlationData);
            }

            if (messageExpiryIntervalSeconds > 0)
            {
                builder.WithMessageExpiryInterval(messageExpiryIntervalSeconds);
            }

            return builder.Build();
        }

        /// <summary>
        /// Reads the <c>type</c> user property, which carries a handler name and a schema version as <c>{name}:{version}</c>.
        /// </summary>
        private static bool TryGetMessageType(MqttApplicationMessage message, [NotNullWhen(true)] out string? type, out int schemaVersion)
        {
            type = null;
            schemaVersion = 0;

            MqttUserProperty? typeProperty = message.UserProperties?.FirstOrDefault(p => p.Name.Equals("type", StringComparison.Ordinal));
            if (typeProperty == null)
            {
                return false;
            }

            string[] parts = typeProperty.ReadValueAsString().Split(':');
            if (parts.Length != 2 || !int.TryParse(parts[1], out schemaVersion))
            {
                return false;
            }

            type = parts[0];
            return true;
        }

        private static bool TryGetCorrelationGuid(MqttApplicationMessage message, out Guid value)
        {
            value = Guid.Empty;

            if (message.CorrelationData == null || message.CorrelationData.Length != 16)
            {
                return false;
            }

            value = new Guid(message.CorrelationData, bigEndian: true);
            return true;
        }

        private static uint ToWholeSeconds(TimeSpan timeSpan)
        {
            return (uint)Math.Max(1, Math.Ceiling(timeSpan.TotalSeconds));
        }

        private sealed class PendingGen2MethodInvocation
        {
            internal TaskCompletionSource<MethodsProto.ProbeAck> ProbeAckReceived { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

            internal TaskCompletionSource<MethodsProto.Result> ResultReceived { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

            internal TaskCompletionSource<MethodsProto.Abandon> Abandoned { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }
}
