using MQTTnet;
using MQTTnet.Protocol;
using System.Buffers;
using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The gen1 (classic IoT hub) half of the stub IoT hub service.
    /// </summary>
    /// <remarks>
    /// Classic IoT hub speaks MQTT 3.1.1, so none of the MQTT 5 metadata the gen2 protocol relies on is available here. Request and
    /// response messages are instead correlated with a <c>$rid</c> query parameter embedded in the topic string, status codes are
    /// carried as a topic segment, and payloads are JSON.
    /// </remarks>
    public sealed partial class StubIotHubService
    {
        internal const string TwinVersionKey = "$version";

        private const string MessagePropertyMessageId = "$.mid";
        private const string MessagePropertyCorrelationId = "$.cid";
        private const string MessagePropertyContentType = "$.ct";
        private const string MessagePropertyContentEncoding = "$.ce";

        private readonly ConcurrentDictionary<string, TaskCompletionSource<Gen1DirectMethodResponse>> _pendingGen1MethodInvocations = new();

        private long _nextGen1RequestId;

        /// <summary>
        /// Send a cloud-to-device message to a device. Gen1 only; the gen2 cloud-to-device protocol has not been defined yet.
        /// </summary>
        public async Task SendCloudToDeviceMessageAsync(string deviceId, StubCloudToDeviceMessage message, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);
            ArgumentException.ThrowIfNullOrEmpty(deviceId);
            ArgumentNullException.ThrowIfNull(message);
            EnsureStarted();

            if (_options.Generation != IotHubGeneration.Gen1)
            {
                throw new NotSupportedException("Cloud-to-device messaging is not supported by a gen2 IoT hub yet.");
            }

            // The classic device-bound topic always carries a property bag, and the device SDK indexes into that topic segment
            // unconditionally, so make sure there is always at least a message id in it.
            var properties = new List<KeyValuePair<string, string>>
            {
                new(MessagePropertyMessageId, message.MessageId ?? Guid.NewGuid().ToString()),
            };

            if (message.CorrelationId != null)
            {
                properties.Add(new(MessagePropertyCorrelationId, message.CorrelationId));
            }

            if (message.ContentType != null)
            {
                properties.Add(new(MessagePropertyContentType, message.ContentType));
            }

            if (message.ContentEncoding != null)
            {
                properties.Add(new(MessagePropertyContentEncoding, message.ContentEncoding));
            }

            foreach (KeyValuePair<string, string> userProperty in message.UserProperties)
            {
                properties.Add(userProperty);
            }

            string topic = $"devices/{deviceId}/messages/devicebound/{BuildTopicPropertyBag(properties)}";

            await PublishAsync(
                new MqttApplicationMessageBuilder()
                    .WithTopic(topic)
                    .WithPayload(message.Payload)
                    .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce)
                    .Build(),
                cancellationToken);
        }

        private async Task HandleGen1MessageAsync(MqttApplicationMessageReceivedEventArgs args)
        {
            MqttApplicationMessage message = args.ApplicationMessage;
            string[] topicSegments = message.Topic.Split('/');

            if (topicSegments.Length >= 4
                && topicSegments[0].Equals("devices", StringComparison.Ordinal)
                && topicSegments[2].Equals("messages", StringComparison.Ordinal)
                && topicSegments[3].Equals("events", StringComparison.Ordinal))
            {
                HandleGen1Telemetry(topicSegments[1], topicSegments.Length > 4 ? topicSegments[4] : string.Empty, message);
                return;
            }

            if (topicSegments.Length < 2 || !topicSegments[0].Equals("$iothub", StringComparison.Ordinal))
            {
                return;
            }

            // $iothub/twin/GET/?$rid={requestId}
            if (topicSegments.Length == 4
                && topicSegments[1].Equals("twin", StringComparison.Ordinal)
                && topicSegments[2].Equals("GET", StringComparison.Ordinal))
            {
                await HandleGen1TwinGetAsync(message, topicSegments[3]);
                return;
            }

            // $iothub/twin/PATCH/properties/reported/?$rid={requestId}
            if (topicSegments.Length == 6
                && topicSegments[1].Equals("twin", StringComparison.Ordinal)
                && topicSegments[2].Equals("PATCH", StringComparison.Ordinal)
                && topicSegments[3].Equals("properties", StringComparison.Ordinal)
                && topicSegments[4].Equals("reported", StringComparison.Ordinal))
            {
                await HandleGen1ReportedPatchAsync(message, topicSegments[5]);
                return;
            }

            // $iothub/methods/res/{status}/?$rid={requestId}
            if (topicSegments.Length == 5
                && topicSegments[1].Equals("methods", StringComparison.Ordinal)
                && topicSegments[2].Equals("res", StringComparison.Ordinal))
            {
                HandleGen1DirectMethodResponse(message, topicSegments[3], topicSegments[4]);
                return;
            }

            Log($"Discarding a gen1 message on unrecognized topic '{message.Topic}'.");
        }

        private void HandleGen1Telemetry(string deviceId, string propertyBag, MqttApplicationMessage message)
        {
            if (!IsServedDevice(deviceId))
            {
                return;
            }

            Dictionary<string, string> properties = ParseTopicPropertyBag(propertyBag);
            var userProperties = new Dictionary<string, string>(StringComparer.Ordinal);
            string? messageId = null;
            string? correlationId = null;
            string? contentType = null;
            string? contentEncoding = null;

            foreach (KeyValuePair<string, string> property in properties)
            {
                switch (property.Key)
                {
                    case MessagePropertyMessageId:
                        messageId = property.Value;
                        break;

                    case MessagePropertyCorrelationId:
                        correlationId = property.Value;
                        break;

                    case MessagePropertyContentType:
                        contentType = property.Value;
                        break;

                    case MessagePropertyContentEncoding:
                        contentEncoding = property.Value;
                        break;

                    default:
                        userProperties[property.Key] = property.Value;
                        break;
                }
            }

            TelemetryReceived?.Invoke(this, new StubTelemetryReceivedEventArgs
            {
                DeviceId = deviceId,
                Payload = message.Payload.ToArray(),
                MessageId = messageId,
                CorrelationId = correlationId,
                ContentType = contentType,
                ContentEncoding = contentEncoding,
                UserProperties = userProperties,
            });
        }

        private async Task HandleGen1TwinGetAsync(MqttApplicationMessage message, string propertyBag)
        {
            // Classic IoT hub scopes the twin topics per connection rather than per topic string, so the device id is not in the
            // topic. The stub recovers it from the requesting MQTT session's client id, which the device SDK sets to the device id.
            string? deviceId = ResolveGen1DeviceId(message);
            if (deviceId == null)
            {
                return;
            }

            if (!ParseTopicPropertyBag(propertyBag).TryGetValue("$rid", out string? requestId))
            {
                Log($"Discarding a gen1 twin GET on '{message.Topic}' because it carries no $rid.");
                return;
            }

            StubTwinSnapshot twin = GetDeviceState(deviceId).GetTwinSnapshot();

            // Classic IoT hub returns the whole twin, with each section's version inlined as a "$version" metadata key.
            var desired = (JsonObject)twin.Desired.DeepClone();
            desired[TwinVersionKey] = twin.DesiredVersion;

            var reported = (JsonObject)twin.Reported.DeepClone();
            reported[TwinVersionKey] = twin.ReportedVersion;

            var body = new JsonObject
            {
                ["desired"] = desired,
                ["reported"] = reported,
            };

            await PublishAsync(
                new MqttApplicationMessageBuilder()
                    .WithTopic($"$iothub/twin/res/200/?$rid={Uri.EscapeDataString(requestId)}")
                    .WithPayload(Encoding.UTF8.GetBytes(body.ToJsonString()))
                    .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtMostOnce)
                    .Build(),
                CancellationToken.None);

            TwinGetReceived?.Invoke(this, new StubTwinGetReceivedEventArgs
            {
                DeviceId = deviceId,

                // Classic IoT hub has no per-section filtering; a GET always returns both sections.
                RequestedDesired = true,
                RequestedReported = true,
            });
        }

        private async Task HandleGen1ReportedPatchAsync(MqttApplicationMessage message, string propertyBag)
        {
            string? deviceId = ResolveGen1DeviceId(message);
            if (deviceId == null)
            {
                return;
            }

            if (!ParseTopicPropertyBag(propertyBag).TryGetValue("$rid", out string? requestId))
            {
                Log($"Discarding a gen1 reported patch on '{message.Topic}' because it carries no $rid.");
                return;
            }

            JsonObject patch;
            try
            {
                patch = JsonNode.Parse(Encoding.UTF8.GetString(message.Payload.ToArray()))?.AsObject() ?? new JsonObject();
            }
            catch (System.Text.Json.JsonException)
            {
                await PublishAsync(
                    new MqttApplicationMessageBuilder()
                        .WithTopic($"$iothub/twin/res/400/?$rid={Uri.EscapeDataString(requestId)}")
                        .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtMostOnce)
                        .Build(),
                    CancellationToken.None);
                return;
            }

            // Classic IoT hub reported writes are unconditional, so there is no if-match to honor.
            StubDeviceState device = GetDeviceState(deviceId);
            device.TryApplyReportedPatch(patch, ifMatch: 0, out ulong newVersion);

            await PublishAsync(
                new MqttApplicationMessageBuilder()
                    .WithTopic($"$iothub/twin/res/204/?$rid={Uri.EscapeDataString(requestId)}&{TwinVersionKey}={newVersion.ToString(CultureInfo.InvariantCulture)}")
                    .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtMostOnce)
                    .Build(),
                CancellationToken.None);

            ReportedPropertiesReceived?.Invoke(this, new StubReportedPropertiesReceivedEventArgs
            {
                DeviceId = deviceId,
                Patch = patch,
                IfMatch = 0,
                WasApplied = true,
                ReportedVersion = newVersion,
            });
        }

        private void HandleGen1DirectMethodResponse(MqttApplicationMessage message, string statusSegment, string propertyBag)
        {
            if (!ParseTopicPropertyBag(propertyBag).TryGetValue("$rid", out string? requestId))
            {
                Log($"Discarding a gen1 direct method response on '{message.Topic}' because it carries no $rid.");
                return;
            }

            if (!_pendingGen1MethodInvocations.TryGetValue(requestId, out TaskCompletionSource<Gen1DirectMethodResponse>? pending))
            {
                // The invocation already timed out. A late response is simply ignored.
                return;
            }

            if (!int.TryParse(statusSegment, NumberStyles.Integer, CultureInfo.InvariantCulture, out int status))
            {
                Log($"Discarding a gen1 direct method response on '{message.Topic}' because its status segment is not an integer.");
                return;
            }

            pending.TrySetResult(new Gen1DirectMethodResponse(status, message.Payload.ToArray()));
        }

        private async Task<StubDirectMethodResult> InvokeGen1DirectMethodAsync(
            string deviceId,
            string methodName,
            byte[]? payload,
            TimeSpan responseTimeout,
            CancellationToken cancellationToken)
        {
            string requestId = Interlocked.Increment(ref _nextGen1RequestId).ToString(CultureInfo.InvariantCulture);
            var pending = new TaskCompletionSource<Gen1DirectMethodResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
            _pendingGen1MethodInvocations[requestId] = pending;

            try
            {
                await PublishAsync(
                    new MqttApplicationMessageBuilder()
                        .WithTopic($"$iothub/methods/POST/{methodName}/?$rid={requestId}")
                        .WithPayload(payload ?? Array.Empty<byte>())
                        .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtMostOnce)
                        .Build(),
                    cancellationToken);

                Gen1DirectMethodResponse response;
                try
                {
                    response = await pending.Task.WaitAsync(responseTimeout, cancellationToken);
                }
                catch (TimeoutException)
                {
                    return new StubDirectMethodResult { Outcome = StubDirectMethodOutcome.TimedOut };
                }

                return new StubDirectMethodResult
                {
                    Outcome = StubDirectMethodOutcome.Completed,
                    Status = response.Status,
                    Payload = response.Payload,
                };
            }
            finally
            {
                _pendingGen1MethodInvocations.TryRemove(requestId, out _);
            }
        }

        private Task PublishGen1DesiredPatchAsync(string deviceId, JsonObject patch, ulong newVersion, CancellationToken cancellationToken)
        {
            if (!IsServedDevice(deviceId))
            {
                return Task.CompletedTask;
            }

            // Classic IoT hub inlines the new desired version into the patch body rather than into a protobuf field.
            var body = (JsonObject)patch.DeepClone();
            body[TwinVersionKey] = newVersion;

            return PublishAsync(
                new MqttApplicationMessageBuilder()
                    .WithTopic($"$iothub/twin/PATCH/properties/desired/?{TwinVersionKey}={newVersion.ToString(CultureInfo.InvariantCulture)}")
                    .WithPayload(Encoding.UTF8.GetBytes(body.ToJsonString()))
                    .WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtMostOnce)
                    .Build(),
                cancellationToken);
        }

        /// <summary>
        /// Determines which device sent a message on one of the connection-scoped classic topics.
        /// </summary>
        /// <remarks>
        /// The classic <c>$iothub/...</c> topics carry no device id, because a real hub knows which device a message came from by
        /// the MQTT session it arrived on. A stub attached to a shared broker cannot see that, so it relies on
        /// <see cref="StubIotHubServiceOptions.DeviceIdFilter"/> being set, or on exactly one device having been observed so far.
        /// </remarks>
        private string? ResolveGen1DeviceId(MqttApplicationMessage message)
        {
            if (_options.DeviceIdFilter != null)
            {
                return _options.DeviceIdFilter;
            }

            string[] knownDeviceIds = _devices.Keys.ToArray();
            if (knownDeviceIds.Length == 1)
            {
                return knownDeviceIds[0];
            }

            Log($"Cannot attribute the gen1 message on '{message.Topic}' to a device. The classic twin and direct method topics "
                + $"carry no device id, so set {nameof(StubIotHubServiceOptions)}.{nameof(StubIotHubServiceOptions.DeviceIdFilter)} "
                + "when more than one device shares the broker.");
            return null;
        }

        private static string BuildTopicPropertyBag(IEnumerable<KeyValuePair<string, string>> properties)
        {
            return string.Join('&', properties.Select(p => $"{Uri.EscapeDataString(p.Key)}={Uri.EscapeDataString(p.Value)}"));
        }

        private readonly record struct Gen1DirectMethodResponse(int Status, byte[] Payload);
    }
}
