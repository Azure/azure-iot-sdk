// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Devices;
using Microsoft.Azure.Iot.Device.IntegrationTests.Unified;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Models.Twin;
using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;
using ServiceTwin = Microsoft.Azure.Devices.Shared.Twin;
using TwinResult = Microsoft.Azure.Iot.Device.Models.Twin.Result;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Longhaul
{
    /// <summary>
    /// Multi-hour stability scenarios: one device stays connected for <see cref="LonghaulSettings.Duration"/> while it
    /// continuously sends telemetry, reports properties, receives desired property patches, answers direct methods and
    /// receives cloud-to-device messages. Each scenario writes a JSON and a markdown analysis
    /// to <see cref="LonghaulSettings.OutputDirectory"/> and fails if the analysis breaches a threshold.
    /// </summary>
    /// <remarks>
    /// Skipped unless <c>LONGHAUL_DURATION_MINUTES</c> is set. The longhaul-dotnet workflow runs these; the regular e2e
    /// workflows exclude <c>Category=Longhaul</c>.
    /// </remarks>
    [Trait("Category", "Longhaul")]
    public class LonghaulScenarioTests
    {
        public const string ScenarioTrait = "LonghaulScenario";

        [Fact]
        [Trait(ScenarioTrait, "UnifiedMQTTv3Hub")]
        public async Task UnifiedApiOnMQTTv3Hub()
        {
            const string scenario = "UnifiedMQTTv3Hub";
            LonghaulSettings settings = GetSettingsOrSkip();
            CancellationToken cancellationToken = TestContext.Current.CancellationToken;

            // Setup provisions through the default global DPS endpoint; see CreateConnectedUnifiedConnectionClientAsync.
            EndpointTrackingMqttClient mqttClient = new(new ProvisioningSettings(Setup.DpsIdScope).GlobalEndpointAddress);
            Device.Unified.Models.ConnectionClientOptions options = new()
            {
                MqttClient = mqttClient,
            };

            UnifiedDeviceTestContext device = await Setup.CreateConnectedUnifiedConnectionClientAsync(
                testAgainstClassicHub: true,
                options: options,
                certificateValidity: CertificateValidity(settings),
                cancellationToken: cancellationToken);
            string deviceId = device.ConnectionContext.DeviceId;
            using Device.Unified.Telemetry.TelemetryClient telemetryClient = new(device.ConnectionClient);
            using Device.Unified.Twin.TwinClient twinClient = new(device.ConnectionClient);
            using Device.Unified.DirectMethods.DirectMethodClient directMethodClient = new(device.ConnectionClient);
            using RegistryManager registryManager = Setup.GetMQTTv3IotHubRegistryManager();
            using ServiceClient serviceClient = Setup.GetMQTTv3IotHubServiceClient();
            using CloudToDeviceSender cloudToDeviceSender = new();

            await using (device)
            {
                HighWaterMark desiredSequence = new();
                ConcurrentDictionary<string, TaskCompletionSource> pendingCloudToDevice = new();
                twinClient.DesiredPatchReceived += args => DeviceOperations.ObserveDesired(args.DesiredProperties, desiredSequence);
                // After a reconnect or a missed patch, the client delivers the desired properties as a snapshot instead.
                twinClient.TwinPushReceived += args =>
                {
                    if (args.Desired != null)
                    {
                        DeviceOperations.ObserveDesired(args.Desired.Properties, desiredSequence);
                    }
                };
                directMethodClient.DirectMethodInvokedAsync += DeviceOperations.RespondToDirectMethodAsync;
                telemetryClient.CloudToDeviceTelemetryReceivedAsync += message =>
                {
                    if (message.MessageId != null && pendingCloudToDevice.TryRemove(message.MessageId, out TaskCompletionSource? received))
                    {
                        received.TrySetResult();
                    }

                    return Task.CompletedTask;
                };

                LonghaulRun run = new(settings, scenario, "Unified", "MQTTv3", Log());
                run.Observe(device.ConnectionClient, mqttClient);
                run.AddOperation("Telemetry", settings.TelemetryInterval,
                    (sequence, ct) => telemetryClient.SendTelemetryAsync(DeviceOperations.CreateTelemetry(scenario, sequence), ct));
                run.AddOperation("Reported properties", settings.ReportedPropertiesInterval, async (sequence, ct) =>
                    DeviceOperations.ThrowIfRejected(await twinClient.UpdateReportedPropertiesAsync(DeviceOperations.CreateReportedPatch(sequence), ct)));
                run.AddOperation("Desired properties", settings.DesiredPropertiesInterval,
                    (sequence, ct) => ServiceOperations.UpdateDesiredAsync(registryManager, deviceId, sequence, desiredSequence, ct));
                run.AddOperation("Direct methods", settings.DirectMethodInterval,
                    (sequence, ct) => ServiceOperations.InvokeDirectMethodAsync(serviceClient, deviceId, sequence, settings.OperationTimeout, ct));
                run.AddOperation("Cloud-to-device messages", settings.CloudToDeviceInterval,
                    (sequence, ct) => cloudToDeviceSender.SendAndAwaitDeliveryAsync(deviceId, sequence, pendingCloudToDevice, ct));

                await RunAndAnalyzeAsync(run, settings, cancellationToken);
            }
        }

        private static LonghaulSettings GetSettingsOrSkip()
        {
            LonghaulSettings? settings = LonghaulSettings.FromEnvironment();
            if (settings == null)
            {
                Assert.Skip($"{LonghaulSettings.DurationVariable} is not set; longhaul scenarios only run when a duration is configured.");
            }

            return settings;
        }

        // The test certificates must outlive the run: the device presents its certificate again on every reconnect.
        private static TimeSpan CertificateValidity(LonghaulSettings settings) => settings.Duration + TimeSpan.FromHours(2);

        private static Action<string> Log()
        {
            ITestContext context = TestContext.Current;
            return message =>
            {
                string line = $"{DateTimeOffset.UtcNow:HH:mm:ss} {message}";
                context.SendDiagnosticMessage(line);
                try
                {
                    context.TestOutputHelper?.WriteLine(line);
                }
                catch (InvalidOperationException)
                {
                    // The test finished; its output can no longer be written.
                }
            };
        }

        private static async Task RunAndAnalyzeAsync(LonghaulRun run, LonghaulSettings settings, CancellationToken cancellationToken)
        {
            LonghaulReport report = await run.RunAsync(cancellationToken);
            report.WriteTo(settings.OutputDirectory);
            TestContext.Current.TestOutputHelper?.WriteLine(report.ToMarkdown());

            Assert.True(report.Passed, $"Longhaul scenario {report.Scenario} breached its thresholds:{Environment.NewLine}{string.Join(Environment.NewLine, report.Violations)}");
        }

        /// <summary>
        /// Tracks the highest sequence number seen, so a waiter for sequence N is satisfied by N or anything later. A
        /// device that reconnects may receive the whole desired document instead of every patch it missed, so only the
        /// latest value is guaranteed to arrive.
        /// </summary>
        private sealed class HighWaterMark
        {
            private readonly object _lock = new();
            private readonly List<(long Target, TaskCompletionSource Reached)> _waiters = new();
            private long _value;

            public void Observe(long value)
            {
                List<TaskCompletionSource> reached = new();
                lock (_lock)
                {
                    if (value <= _value)
                    {
                        return;
                    }

                    _value = value;
                    _waiters.RemoveAll(waiter =>
                    {
                        if (waiter.Target > value)
                        {
                            return false;
                        }

                        reached.Add(waiter.Reached);
                        return true;
                    });
                }

                reached.ForEach(r => r.TrySetResult());
            }

            public async Task WaitForAsync(long target, CancellationToken cancellationToken)
            {
                TaskCompletionSource reached = new(TaskCreationOptions.RunContinuationsAsynchronously);
                lock (_lock)
                {
                    if (_value >= target)
                    {
                        return;
                    }

                    _waiters.Add((target, reached));
                }

                try
                {
                    await reached.Task.WaitAsync(cancellationToken);
                }
                finally
                {
                    lock (_lock)
                    {
                        _waiters.RemoveAll(waiter => waiter.Reached == reached);
                    }
                }
            }
        }

        private static class DeviceOperations
        {
            public const string DesiredSequenceProperty = "longhaulSequence";

            public static DeviceToCloudTelemetry CreateTelemetry(string scenario, long sequence) => new()
            {
                Payload = JsonSerializer.SerializeToUtf8Bytes(new { scenario, sequence, sentUtc = DateTimeOffset.UtcNow }),
                ContentType = "application/json",
                ContentEncoding = "utf-8",
                MessageId = Guid.NewGuid().ToString(),
            };

            public static JsonObject CreateReportedPatch(long sequence) => new()
            {
                ["longhaul"] = new JsonObject
                {
                    ["sequence"] = sequence,
                    ["reportedUtc"] = DateTimeOffset.UtcNow.ToString("O", CultureInfo.InvariantCulture),
                },
            };

            public static void ThrowIfRejected(ReportedPatchResponse response)
            {
                if (response.Result != TwinResult.Ok)
                {
                    throw new InvalidOperationException($"IoT hub rejected the reported properties patch: {response.Result}.");
                }
            }

            public static void ObserveDesired(JsonObject desiredProperties, HighWaterMark desiredSequence)
            {
                if (desiredProperties[DesiredSequenceProperty] is JsonValue value && value.TryGetValue(out long sequence))
                {
                    desiredSequence.Observe(sequence);
                }
            }

            /// <summary>
            /// Echoes the request payload so that the caller can confirm it got the response to its own invocation.
            /// </summary>
            public static Task<DirectMethodResponse> RespondToDirectMethodAsync(DirectMethodRequestReceivedEventArgs args) =>
                Task.FromResult(args.MethodName == ServiceOperations.DirectMethodName
                    ? new DirectMethodResponse { Status = 200, Payload = args.Payload }
                    : new DirectMethodResponse { Status = 404 });
        }

        private static class ServiceOperations
        {
            public const string DirectMethodName = "longhaul";

            public static async Task UpdateDesiredAsync(RegistryManager registryManager, string deviceId, long sequence, HighWaterMark desiredSequence, CancellationToken cancellationToken)
            {
                ServiceTwin patch = new();
                patch.Properties.Desired[DeviceOperations.DesiredSequenceProperty] = sequence;
                await registryManager.UpdateTwinAsync(deviceId, patch, "*", cancellationToken);
                await desiredSequence.WaitForAsync(sequence, cancellationToken);
            }

            public static async Task InvokeDirectMethodAsync(ServiceClient serviceClient, string deviceId, long sequence, TimeSpan operationTimeout, CancellationToken cancellationToken)
            {
                // Leave room inside the operation timeout for the service call itself.
                TimeSpan timeout = TimeSpan.FromSeconds(Math.Max(5, (operationTimeout.TotalSeconds - 10) / 2));
                CloudToDeviceMethod method = new(DirectMethodName, timeout, timeout);
                string payload = new JsonObject { ["sequence"] = sequence }.ToJsonString();
                method.SetPayloadJson(payload);

                CloudToDeviceMethodResult result = await serviceClient.InvokeDeviceMethodAsync(deviceId, method, cancellationToken);
                if (result.Status != 200)
                {
                    throw new InvalidOperationException($"The device answered direct method #{sequence} with status {result.Status}.");
                }

                if (JsonNode.Parse(result.GetPayloadAsJson())?["sequence"]?.GetValue<long>() != sequence)
                {
                    throw new InvalidOperationException($"The response to direct method #{sequence} did not echo its payload: {result.GetPayloadAsJson()}");
                }
            }
        }

        /// <summary>
        /// Owns the service client used for cloud-to-device messages. Its AMQP link is long-lived, so it is replaced after
        /// a failed send to keep a broken service-side link from being reported as device failures for the rest of the run.
        /// </summary>
        private sealed class CloudToDeviceSender : IDisposable
        {
            private ServiceClient _serviceClient = Create();

            private ServiceClient ServiceClient => Volatile.Read(ref _serviceClient);

            public async Task SendAndAwaitDeliveryAsync(string deviceId, long sequence, ConcurrentDictionary<string, TaskCompletionSource> pending, CancellationToken cancellationToken)
            {
                string messageId = $"longhaul-{sequence}-{Guid.NewGuid()}";
                TaskCompletionSource delivered = new(TaskCreationOptions.RunContinuationsAsynchronously);
                pending[messageId] = delivered;
                try
                {
                    using Message message = new(Encoding.UTF8.GetBytes(new JsonObject { ["sequence"] = sequence }.ToJsonString()))
                    {
                        MessageId = messageId,
                        ContentType = "application/json",
                        ContentEncoding = "utf-8",
                        // Don't let undelivered messages from failed attempts pile up in the device's queue.
                        ExpiryTimeUtc = DateTime.UtcNow.AddMinutes(5),
                    };

                    try
                    {
                        await ServiceClient.SendAsync(deviceId, message).WaitAsync(cancellationToken);
                    }
                    catch (Exception)
                    {
                        // Includes this attempt timing out: WaitAsync leaves the send running on a link that may be stuck,
                        // so replace the client (disposing the old one aborts that send) before the next attempt. At the
                        // end of the run this just swaps in an unused client, which Dispose then releases.
                        ServiceClient broken = Interlocked.Exchange(ref _serviceClient, Create());
                        broken.Dispose();
                        throw;
                    }

                    await delivered.Task.WaitAsync(cancellationToken);
                }
                finally
                {
                    pending.TryRemove(messageId, out _);
                }
            }

            public void Dispose() => ServiceClient.Dispose();

            private static ServiceClient Create() => Setup.GetMQTTv3IotHubServiceClient();
        }
    }
}
