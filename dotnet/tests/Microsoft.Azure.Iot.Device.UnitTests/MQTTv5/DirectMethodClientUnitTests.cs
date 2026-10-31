// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods;
using Microsoft.Azure.Iot.Device.MQTTv5.Models;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Models;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using Xunit;
using UnifiedConnectionClient = Microsoft.Azure.Iot.Device.Unified.Connection.ConnectionClient;
using UnifiedDirectMethodClient = Microsoft.Azure.Iot.Device.Unified.DirectMethods.DirectMethodClient;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class DirectMethodClientUnitTests
    {
        private const string DeviceId = "someDeviceId";
        private const string IncomingMethodsTopic = "ih/someDeviceId/dev/methods";
        // The device ID is left for the connection client to fill in when it actually sends the publish.
        private const string OutgoingMethodsTopic = "ih/" + IConnectionClient.DeviceIdPlaceholder + "/srv/methods";

        [Fact]
        public void DirectMethodInvokedAsync_RejectsMultipleHandlers()
        {
            MockFeatureConnectionClient connection = new();
            using DirectMethodClient directMethodClient = new(connection);
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> handler =
                _ => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            Assert.Throws<InvalidOperationException>(() => directMethodClient.DirectMethodInvokedAsync += handler + handler);
            directMethodClient.DirectMethodInvokedAsync += handler;
            Assert.Throws<InvalidOperationException>(() => directMethodClient.DirectMethodInvokedAsync += handler);

            directMethodClient.DirectMethodInvokedAsync -= handler;
            directMethodClient.DirectMethodInvokedAsync += handler;
        }

        [Fact]
        public void DirectMethodProbeReceivedAsync_RejectsMultipleHandlers()
        {
            MockFeatureConnectionClient connection = new();
            using DirectMethodClient directMethodClient = new(connection);
            Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>> handler =
                _ => Task.FromResult(DirectMethodProbeAck.Accepted());

            Assert.Throws<InvalidOperationException>(() => directMethodClient.DirectMethodProbeReceivedAsync += handler + handler);
            directMethodClient.DirectMethodProbeReceivedAsync += handler;
            Assert.Throws<InvalidOperationException>(() => directMethodClient.DirectMethodProbeReceivedAsync += handler);

            directMethodClient.DirectMethodProbeReceivedAsync -= handler;
            directMethodClient.DirectMethodProbeReceivedAsync += handler;
        }

        [Fact]
        public async Task ProbeThenExec_InvokesHandlerAndPublishesResult()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            byte[] responseBody = Encoding.UTF8.GetBytes("done");
            DirectMethodRequestReceivedEventArgs? invokedArgs = null;

            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (args) =>
            {
                invokedArgs = args;
                return Task.FromResult(new DirectMethodResponse() { Status = 200, Payload = responseBody });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();

            // 1. Deliver the probe and assert the client acknowledges readiness.
            var probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()));

            MqttPublish probeAckPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingMethodsTopic, probeAckPublish.Topic);
            Assert.Equal("probe-ack:1", GetType(probeAckPublish));

            ProbeAck sentProbeAck = ProbeAck.Parser.ParseFrom(probeAckPublish.Payload);
            Assert.NotNull(sentProbeAck.Ready);
            ByteString readyId = sentProbeAck.Ready.ReadyId;

            // 2. Deliver the exec that references the ready id, and assert the result is published.
            var exec = new Exec() { ReadyId = readyId, Params = ByteString.CopyFromUtf8("{\"delaySeconds\":0}") };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.Equal(2, connection.PublishedMessages.Count);
            MqttPublish resultPublish = connection.PublishedMessages[1];
            Assert.Equal(OutgoingMethodsTopic, resultPublish.Topic);
            Assert.Equal("result:1", GetType(resultPublish));

            Result result = Result.Parser.ParseFrom(resultPublish.Payload);
            Assert.Equal(200, result.Status);
            Assert.Equal(responseBody, result.Body.ToByteArray());

            Assert.NotNull(invokedArgs);
            Assert.Equal("reboot", invokedArgs!.MethodName);
            Assert.Equal("{\"delaySeconds\":0}", Encoding.UTF8.GetString(invokedArgs.Payload!));
            Assert.True(invokedArgs.CancellationToken.CanBeCanceled);
            Assert.False(invokedArgs.CancellationToken.IsCancellationRequested);
        }

        [Theory]
        [InlineData(null, 200)]
        [InlineData(0, 0)]
        [InlineData(128 * 1024, int.MinValue)]
        [InlineData(128 * 1024, int.MaxValue)]
        public async Task Exec_ResponseWithinLimits_PublishesResult(int? payloadSize, int status)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            byte[]? payload = payloadSize.HasValue ? new byte[payloadSize.Value] : null;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = status, Payload = payload });

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData);
            var exec = new Exec() { ReadyId = readyId };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.Equal(2, connection.PublishedMessages.Count);
            MqttPublish resultPublish = connection.PublishedMessages[1];
            Assert.Equal("result:1", GetType(resultPublish));
            Result result = Result.Parser.ParseFrom(resultPublish.Payload);
            Assert.Equal(status, result.Status);
            Assert.Equal(payload ?? Array.Empty<byte>(), result.Body.ToByteArray());
        }

        [Fact]
        public async Task Exec_ResponsePayloadExceedsLimit_DoesNotPublishResult()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse()
            {
                Status = 200,
                Payload = new byte[128 * 1024 + 1],
            });

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData);
            var exec = new Exec() { ReadyId = readyId };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.Single(connection.PublishedMessages);
        }

        [Theory]
        [InlineData(false, false)]
        [InlineData(true, false)]
        [InlineData(false, true)]
        public async Task Probe_WithoutBothHandlers_RejectsAsMethodNotFound(bool registerProbeHandler, bool registerMethodHandler)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            bool probeHandlerInvoked = false;
            if (registerProbeHandler)
            {
                directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
                {
                    probeHandlerInvoked = true;
                    return Task.FromResult(DirectMethodProbeAck.Accepted());
                };
            }

            if (registerMethodHandler)
            {
                directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });
            }

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            var probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 };
            MockMqttPublishReceivedEventArgs args = await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()));

            Assert.Equal(0, args.AcknowledgeCount);
            Assert.False(probeHandlerInvoked);

            MqttPublish probeAckPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingMethodsTopic, probeAckPublish.Topic);
            Assert.Equal("probe-ack:1", GetType(probeAckPublish));
            Assert.Equal(correlationData, probeAckPublish.CorrelationData);

            ProbeAck sentProbeAck = ProbeAck.Parser.ParseFrom(probeAckPublish.Payload);
            Assert.Equal(ProbeAck.ResultOneofCase.Rejected, sentProbeAck.ResultCase);
            Assert.Equal(RejectedReason.MethodNotFound, sentProbeAck.Rejected.Reason);
        }

        [Fact]
        public async Task DirectMethodTrafficIsAcknowledgedByTheConnectionManager()
        {
            using MockConnectionMqttClient mqttClient = new() { AutoRespondToBirth = true };
            using UnifiedConnectionClient connectionClient = new(new() { MqttClient = mqttClient });
            UnifiedDirectMethodClient directMethodClient = new(connectionClient);
            try
            {
                TaskCompletionSource methodInvoked = new(TaskCreationOptions.RunContinuationsAsynchronously);
                directMethodClient.DirectMethodInvokedAsync += _ =>
                {
                    methodInvoked.TrySetResult();
                    return Task.FromResult(new DirectMethodResponse() { Status = 200 });
                };

                await connectionClient.ConnectToHubAsync(
                    MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
                    TestContext.Current.CancellationToken);

                byte[] correlationData = Guid.NewGuid().ToByteArray();
                var probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 };
                MockMqttPublishReceivedEventArgs probeArgs = new()
                {
                    RejectRepeatedAcknowledgements = true,
                    Publish = CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()),
                };
                await mqttClient.SimulatePublishReceivedEventAsync(probeArgs);
                Assert.Equal(1, probeArgs.AcknowledgeCount);

                // The probe ack is sent in the background.
                MqttPublish? probeAckPublish = null;
                await WaitUntilAsync(() =>
                {
                    lock (mqttClient.SentMqttTrafficInOrder)
                    {
                        probeAckPublish = mqttClient.SentMqttTrafficInOrder
                            .Where(traffic => traffic.Publish?.Topic == "ih/" + DeviceId + "/srv/methods")
                            .Select(traffic => traffic.Publish!)
                            .SingleOrDefault();
                    }

                    return probeAckPublish != null;
                });
                ByteString readyId = ProbeAck.Parser.ParseFrom(probeAckPublish!.Payload).Ready.ReadyId;

                var exec = new Exec() { ReadyId = readyId };
                MockMqttPublishReceivedEventArgs execArgs = new()
                {
                    RejectRepeatedAcknowledgements = true,
                    Publish = CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()),
                };
                await mqttClient.SimulatePublishReceivedEventAsync(execArgs);
                Assert.Equal(1, execArgs.AcknowledgeCount);

                await methodInvoked.Task.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken);
            }
            finally
            {
                directMethodClient.Dispose(false);
            }
        }

        [Fact]
        public async Task Exec_WithUnknownRequestId_IsIgnored()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            // Exec arrives without a preceding probe, so there is no correlated ready id and no result should be sent.
            var exec = new Exec() { ReadyId = ByteString.CopyFrom(Guid.NewGuid().ToByteArray()) };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", Guid.NewGuid().ToByteArray(), exec.ToByteArray()));

            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task HandleReceivedMqttPublish_IgnoresUnrelatedTopics()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            bool invoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                invoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            await ReceiveAsync(connection, directMethodClient, new MqttPublish()
            {
                Topic = "ih/someDeviceId/dev/twin",
                Payload = Encoding.UTF8.GetBytes("irrelevant"),
            });

            Assert.False(invoked);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task Probe_WithMalformedPayload_IsLoggedAndIgnored()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 1 });

            bool probeHandlerInvoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
            {
                probeHandlerInvoked = true;
                return Task.FromResult(DirectMethodProbeAck.Accepted());
            };
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            Guid requestId = Guid.NewGuid();
            byte[] correlationData = requestId.ToByteArray(bigEndian: true);

            using var listener = new CapturingTraceListener(requestId.ToString());
            Trace.Listeners.Add(listener);
            try
            {
                // Field 1 (method_name) declares 5 bytes of data, but none follow, so this is a truncated protobuf message.
                byte[] malformedProbePayload = [0x0A, 0x05];
                await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("probe:1", correlationData, malformedProbePayload));
            }
            finally
            {
                Trace.Listeners.Remove(listener);
            }

            Assert.Contains(listener.Messages, m => m.Contains("malformed payload", StringComparison.Ordinal));
            Assert.False(probeHandlerInvoked);
            Assert.Empty(connection.PublishedMessages);

            // Nothing was held for the malformed probe, so a well-formed redelivery is answered.
            await AssertProbeAcceptedAsync(connection, correlationData);
        }

        [Fact]
        public async Task Exec_WithMalformedPayload_LogsErrorAndKeepsRequestPending()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            bool methodInvoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            Guid requestId = Guid.NewGuid();
            // Request Ids are encoded big-endian on the wire.
            byte[] correlationData = requestId.ToByteArray(bigEndian: true);
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData);

            using var listener = new CapturingTraceListener(requestId.ToString());
            Trace.Listeners.Add(listener);
            try
            {
                // Field 1 (ready_id) declares 5 bytes of data, but none follow, so this is a truncated protobuf message.
                byte[] malformedExecPayload = [0x0A, 0x05];
                await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, malformedExecPayload));
            }
            finally
            {
                Trace.Listeners.Remove(listener);
            }

            Assert.Contains(listener.Messages, m => m.Contains("malformed payload", StringComparison.Ordinal));
            Assert.False(methodInvoked);
            Assert.Single(connection.PublishedMessages);

            // The request is still pending, so a well-formed exec for it afterwards is handled.
            var exec = new Exec() { ReadyId = readyId };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.True(methodInvoked);
            Assert.Equal(2, connection.PublishedMessages.Count);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[1]));
        }

        [Fact]
        public async Task Exec_WithMismatchedReadyId_IsIgnoredAndKeepsRequestReady()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int methodInvocations = 0;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                Interlocked.Increment(ref methodInvocations);
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData);

            var staleExec = new Exec() { ReadyId = ByteString.CopyFrom(Guid.NewGuid().ToByteArray()) };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, staleExec.ToByteArray()));

            Assert.Equal(0, methodInvocations);
            Assert.Single(connection.PublishedMessages);

            var exec = new Exec() { ReadyId = readyId };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.Equal(1, methodInvocations);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[^1]));
        }

        [Fact]
        public async Task Probe_RedeliveredWhileReadyOrAfterExecution_IsIgnored()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            int methodInvocations = 0;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
            {
                Interlocked.Increment(ref probeHandlerInvocations);
                return Task.FromResult(DirectMethodProbeAck.Accepted());
            };
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                Interlocked.Increment(ref methodInvocations);
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData);

            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("probe:1", correlationData, probe));
            Assert.Single(connection.PublishedMessages);

            var exec = new Exec() { ReadyId = readyId };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));
            Assert.Equal(2, connection.PublishedMessages.Count);

            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("probe:1", correlationData, probe));
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.Equal(1, probeHandlerInvocations);
            Assert.Equal(1, methodInvocations);
            Assert.Equal(2, connection.PublishedMessages.Count);
        }

        [Theory]
        // Response timeout 30 s leaves 27 s after the default 3 s safety margin.
        [InlineData(27_000, RejectedReason.InsufficientTime)]
        [InlineData(26_999, null)]
        public async Task Probe_AcceptedWithAMinimumExecutionTime_IsRejectedIfTheResponseTimeoutCannotCoverIt(int minimumExecutionTimeMilliseconds, RejectedReason? expected)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 1 });

            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
                Task.FromResult(DirectMethodProbeAck.Accepted(TimeSpan.FromMilliseconds(minimumExecutionTimeMilliseconds)));
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            Assert.Equal(expected, await SendProbeAsync(connection, Guid.NewGuid().ToByteArray(), responseTimeoutSeconds: 30));
        }

        [Fact]
        public void ProbeAck_RejectsANegativeMinimumExecutionTime()
        {
            Assert.Throws<ArgumentOutOfRangeException>(() => DirectMethodProbeAck.Accepted(TimeSpan.FromTicks(-1)));
        }

        [Fact]
        public async Task Exec_CancelsHandlerTokenAtTheExecutionDeadline()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            CancellationToken handlerToken = default;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += async (args) =>
            {
                handlerToken = args.CancellationToken;
                await Task.Delay(Timeout.Infinite, args.CancellationToken);
                return new DirectMethodResponse() { Status = 200 };
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData, responseTimeoutSeconds: 10);

            // 2 s of exec budget less the 1 s safety margin leaves the handler 1 s.
            var exec = new Exec() { ReadyId = readyId };
            Task receiveTask = ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray(), messageExpiryInterval: 2));

            Assert.Same(receiveTask, await Task.WhenAny(receiveTask, Task.Delay(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken)));
            await receiveTask;

            Assert.True(handlerToken.IsCancellationRequested);
            Assert.Single(connection.PublishedMessages);
        }

        [Theory]
        // Exec budget 3 s less the 3 s safety margin leaves nothing.
        [InlineData(0, 3u)]
        // Exec budget 8 s less the 3 s safety margin leaves exactly the 5 s minimum execution time, which is not enough.
        [InlineData(5, 8u)]
        public async Task Exec_WithTooLittleBudgetLeft_AbandonsWithoutInvokingTheHandler(int minimumExecutionTimeSeconds, uint execBudgetSeconds)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 1 });

            bool methodInvoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
                Task.FromResult(DirectMethodProbeAck.Accepted(TimeSpan.FromSeconds(minimumExecutionTimeSeconds)));
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData);

            var exec = new Exec() { ReadyId = readyId };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray(), messageExpiryInterval: execBudgetSeconds));

            Assert.False(methodInvoked);
            Assert.Equal(2, connection.PublishedMessages.Count);
            AssertAbandon(connection.PublishedMessages[1], correlationData, readyId, AbandonReason.InsufficientTime, expectedExpirySeconds: 30);

            // The abandoned invocation released its capacity and can't be executed by a redelivered exec.
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));
            Assert.False(methodInvoked);
            await AssertProbeAcceptedAsync(connection, Guid.NewGuid().ToByteArray());
        }

        [Theory]
        // Exec budget 30 s less the 3 s safety margin is a 27 s execution deadline. The result keeps the rest of the exec budget.
        [InlineData(26_500, 4u)]
        [InlineData(27_000, null)]
        [InlineData(40_000, null)]
        public async Task Exec_HandlerReturningAtOrAfterTheExecutionDeadline_DoesNotPublishResult(int handlerDurationMilliseconds, uint? expectedResultExpirySeconds)
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            ManualTimeProvider timeProvider = new();
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions(), timeProvider);

            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                // A handler that ignores its cancellation token.
                timeProvider.Advance(TimeSpan.FromMilliseconds(handlerDurationMilliseconds));
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            ByteString readyId = await AssertProbeAcceptedAsync(connection, correlationData);

            var exec = new Exec() { ReadyId = readyId };
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray(), messageExpiryInterval: 30));

            if (expectedResultExpirySeconds.HasValue)
            {
                Assert.Equal(2, connection.PublishedMessages.Count);
                Assert.Equal("result:1", GetType(connection.PublishedMessages[1]));
                Assert.Equal(expectedResultExpirySeconds.Value, connection.PublishedMessages[1].MessageExpiryInterval);
            }
            else
            {
                Assert.Single(connection.PublishedMessages);
            }
        }

        [Fact]
        public void Constructor_RejectsInvalidOptions()
        {
            MockFeatureConnectionClient connection = new();

            Assert.Throws<ArgumentOutOfRangeException>(() => new DirectMethodClient(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 0 }));
            Assert.Throws<ArgumentOutOfRangeException>(() => new DirectMethodClient(connection, new DirectMethodClientOptions() { SafetyMargin = TimeSpan.FromTicks(-1) }));
            Assert.Throws<ArgumentNullException>(() => new DirectMethodClient(connection, null!));
        }

        [Fact]
        public async Task Probe_AtCapacity_IsRejectedAsDeviceBusyUntilAnExecutingHandlerReturns()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 2 });

            TaskCompletionSource handlerStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource handlerCanFinish = new(TaskCreationOptions.RunContinuationsAsynchronously);
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += async (_) =>
            {
                handlerStarted.SetResult();
                await handlerCanFinish.Task;
                return new DirectMethodResponse() { Status = 200 };
            };

            // Fill the limit with one executing invocation and one ready token.
            byte[] executingRequest = Guid.NewGuid().ToByteArray();
            ByteString executingReadyId = await AssertProbeAcceptedAsync(connection, executingRequest);

            // The running handler doesn't hold up the connection's receive path, so later probes are still answered.
            await connection.SimulateReceiveAsync(
                CreateInboundMethodPublish("exec:1", executingRequest, new Exec() { ReadyId = executingReadyId }.ToByteArray()));
            await handlerStarted.Task.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken);
            await AssertProbeAcceptedAsync(connection, Guid.NewGuid().ToByteArray());

            byte[] busyRequest = Guid.NewGuid().ToByteArray();
            Assert.Equal(RejectedReason.DeviceBusy, await SendProbeAsync(connection, busyRequest));
            // A rejection creates no ready token, so a redelivered probe for it is evaluated again rather than ignored as a duplicate.
            Assert.Equal(RejectedReason.DeviceBusy, await SendProbeAsync(connection, busyRequest));

            // The executing invocation stops counting once its handler returns.
            handlerCanFinish.SetResult();
            await directMethodClient.WhenIdleAsync();
            await AssertProbeAcceptedAsync(connection, busyRequest);
        }

        [Fact]
        public async Task ReadyToken_IsAbandonedAndReleasesCapacityWhenItsReadyWaitExpires()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            ManualTimeProvider timeProvider = new();
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 1 }, timeProvider);

            bool methodInvoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] expiringRequest = Guid.NewGuid().ToByteArray();
            ByteString expiringReadyId = await AssertProbeAcceptedAsync(connection, expiringRequest, responseTimeoutSeconds: 40);

            // Remaining connect budget (30 s) + response timeout (40 s) + safety margin (4 s).
            ManualTimer readyWaitTimer = Assert.Single(timeProvider.Timers);
            Assert.Equal(TimeSpan.FromSeconds(74), readyWaitTimer.DueTime);
            Assert.Equal(RejectedReason.DeviceBusy, await SendProbeAsync(connection, Guid.NewGuid().ToByteArray()));

            readyWaitTimer.Fire();
            await directMethodClient.WhenIdleAsync();

            AssertAbandon(connection.PublishedMessages[^1], expiringRequest, expiringReadyId, AbandonReason.ReadyWaitTimeout, expectedExpirySeconds: 40);

            await AssertProbeAcceptedAsync(connection, Guid.NewGuid().ToByteArray());

            // The abandoned token can no longer be executed.
            int publishedBeforeExec = connection.PublishedMessages.Count;
            await ReceiveAsync(
                connection,
                directMethodClient,
                CreateInboundMethodPublish("exec:1", expiringRequest, new Exec() { ReadyId = expiringReadyId }.ToByteArray()));
            Assert.False(methodInvoked);
            Assert.Equal(publishedBeforeExec, connection.PublishedMessages.Count);
        }

        [Fact]
        public async Task ReadyWait_UsesTheConfiguredSafetyMargin()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            ManualTimeProvider timeProvider = new();
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { SafetyMargin = TimeSpan.FromMilliseconds(2500) }, timeProvider);

            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            await AssertProbeAcceptedAsync(connection, Guid.NewGuid().ToByteArray(), responseTimeoutSeconds: 40);

            // Remaining connect budget (30 s) + response timeout (40 s) + configured safety margin (2.5 s).
            Assert.Equal(TimeSpan.FromSeconds(72.5), Assert.Single(timeProvider.Timers).DueTime);
        }

        [Fact]
        public async Task ReadyToken_ReleasesCapacityWhenTheBrokerRejectsItsProbeAck()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 1 });
            directMethodClient.DirectMethodProbeReceivedAsync += (_) => Task.FromResult(DirectMethodProbeAck.Accepted());
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            connection.OnPublish = _ => Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.UnspecifiedError });
            await SendProbeAsync(connection, Guid.NewGuid().ToByteArray());
            await directMethodClient.WhenIdleAsync();

            connection.OnPublish = null;
            await AssertProbeAcceptedAsync(connection, Guid.NewGuid().ToByteArray());
        }

        [Fact]
        public async Task Probe_OverlappingRedeliveries_AreAnsweredOnce()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 1 });

            int probeHandlerInvocations = 0;
            TaskCompletionSource probeHandlerCanFinish = new(TaskCreationOptions.RunContinuationsAsynchronously);
            directMethodClient.DirectMethodProbeReceivedAsync += async (_) =>
            {
                Interlocked.Increment(ref probeHandlerInvocations);
                await probeHandlerCanFinish.Task;
                return DirectMethodProbeAck.Accepted();
            };
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            byte[] request = Guid.NewGuid().ToByteArray();
            byte[] probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", request, probe));
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", request, probe));

            probeHandlerCanFinish.SetResult();
            await directMethodClient.WhenIdleAsync();

            Assert.Equal(1, probeHandlerInvocations);
            MqttPublish probeAckPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(ProbeAck.ResultOneofCase.Ready, ProbeAck.Parser.ParseFrom(probeAckPublish.Payload).ResultCase);
        }

        [Fact]
        public async Task Probe_WhoseHandlerOutlivesTheConnectBudget_IsDroppedWithoutHoldingCapacity()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection, new DirectMethodClientOptions() { MaxConcurrentDirectMethods = 1 });

            TimeSpan probeHandlerDelay = TimeSpan.Zero;
            directMethodClient.DirectMethodProbeReceivedAsync += async (_) =>
            {
                await Task.Delay(probeHandlerDelay);
                return DirectMethodProbeAck.Accepted();
            };
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            // The probe handler takes longer than the 2 s budget, which used to wrap the unsigned remaining budget.
            probeHandlerDelay = TimeSpan.FromSeconds(2.2);
            byte[] probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();
            await ReceiveAsync(connection, directMethodClient, CreateInboundMethodPublish("probe:1", Guid.NewGuid().ToByteArray(), probe, messageExpiryInterval: 2));
            Assert.Empty(connection.PublishedMessages);

            probeHandlerDelay = TimeSpan.Zero;
            await AssertProbeAcceptedAsync(connection, Guid.NewGuid().ToByteArray());
        }

        // Delivers a publish and waits for all the work the client started for it in the background.
        private static async Task<MockMqttPublishReceivedEventArgs> ReceiveAsync(MockFeatureConnectionClient connection, DirectMethodClient directMethodClient, MqttPublish publish)
        {
            MockMqttPublishReceivedEventArgs args = await connection.SimulateReceiveAsync(publish);
            await directMethodClient.WhenIdleAsync();
            return args;
        }

        // Sends a probe and returns its rejection reason, or null if it was accepted.
        private static async Task<RejectedReason?> SendProbeAsync(MockFeatureConnectionClient connection, byte[] correlationData, uint responseTimeoutSeconds = 30)
        {
            int publishedBefore;
            lock (connection.PublishedMessages)
            {
                publishedBefore = connection.PublishedMessages.Count;
            }

            var probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = responseTimeoutSeconds };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()));

            // The probe ack is sent in the background.
            MqttPublish probeAckPublish = await connection.WaitForPublishedMessagesAsync(publishedBefore + 1);
            Assert.Equal("probe-ack:1", GetType(probeAckPublish));
            Assert.Equal(correlationData, probeAckPublish.CorrelationData);
            ProbeAck probeAck = ProbeAck.Parser.ParseFrom(probeAckPublish.Payload);
            return probeAck.ResultCase == ProbeAck.ResultOneofCase.Rejected ? probeAck.Rejected.Reason : null;
        }

        private static async Task<ByteString> AssertProbeAcceptedAsync(MockFeatureConnectionClient connection, byte[] correlationData, uint responseTimeoutSeconds = 30)
        {
            Assert.Null(await SendProbeAsync(connection, correlationData, responseTimeoutSeconds));
            return ProbeAck.Parser.ParseFrom(connection.PublishedMessages[^1].Payload).Ready.ReadyId;
        }

        private static void AssertAbandon(MqttPublish publish, byte[] correlationData, ByteString readyId, AbandonReason reason, uint expectedExpirySeconds)
        {
            Assert.Equal(OutgoingMethodsTopic, publish.Topic);
            Assert.Equal("abandon:1", GetType(publish));
            Assert.Equal("application/protobuf", publish.ContentType);
            Assert.Equal(correlationData, publish.CorrelationData);
            Assert.Equal(expectedExpirySeconds, publish.MessageExpiryInterval);

            Abandon abandon = Abandon.Parser.ParseFrom(publish.Payload);
            Assert.Equal(readyId, abandon.ReadyId);
            Assert.Equal(reason, abandon.Reason);
        }

        private static async Task WaitUntilAsync(Func<bool> condition)
        {
            using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            while (!condition())
            {
                await Task.Delay(10, timeout.Token);
            }
        }

        private static MqttPublish CreateInboundMethodPublish(string type, byte[] correlationData, byte[] payload, uint messageExpiryInterval = 30)
        {
            var publish = new MqttPublish()
            {
                Topic = IncomingMethodsTopic,
                Payload = payload,
                CorrelationData = correlationData,
                MessageExpiryInterval = messageExpiryInterval,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(type)));
            return publish;
        }

        private static string GetType(MqttPublish publish)
        {
            return Assert.Single(publish.UserProperties, p => p.Name == "type").ReadValueAsString();
        }

        // Timers created through this provider only fire when the test says so, and time only moves when the test advances it.
        private sealed class ManualTimeProvider : TimeProvider
        {
            private long _timestamp;

            public ConcurrentQueue<ManualTimer> Timers { get; } = new();

            public override long TimestampFrequency => TimeSpan.TicksPerSecond;

            public override long GetTimestamp() => Interlocked.Read(ref _timestamp);

            public void Advance(TimeSpan duration) => Interlocked.Add(ref _timestamp, duration.Ticks);

            public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period)
            {
                ManualTimer timer = new(callback, state, dueTime);
                Timers.Enqueue(timer);
                return timer;
            }
        }

        private sealed class ManualTimer : ITimer
        {
            private readonly TimerCallback _callback;
            private readonly object? _state;
            private bool _disposed;

            public ManualTimer(TimerCallback callback, object? state, TimeSpan dueTime)
            {
                _callback = callback;
                _state = state;
                DueTime = dueTime;
            }

            public TimeSpan DueTime { get; private set; }

            public void Fire()
            {
                if (!_disposed)
                {
                    _callback(_state);
                }
            }

            public bool Change(TimeSpan dueTime, TimeSpan period)
            {
                if (_disposed)
                {
                    return false;
                }

                DueTime = dueTime;
                return true;
            }

            public void Dispose() => _disposed = true;

            public ValueTask DisposeAsync()
            {
                Dispose();
                return ValueTask.CompletedTask;
            }
        }

        // Trace listeners are process-wide, so only keep messages that mention this test's request Id.
        private sealed class CapturingTraceListener : TraceListener
        {
            private readonly string _filter;

            public CapturingTraceListener(string filter)
            {
                _filter = filter;
            }

            public ConcurrentQueue<string> Messages { get; } = new();

            public override void Write(string? message) => Capture(message);

            public override void WriteLine(string? message) => Capture(message);

            public override void TraceEvent(TraceEventCache? eventCache, string source, TraceEventType eventType, int id, string? format, params object?[]? args)
            {
                Capture(args == null || format == null ? format : string.Format(format, args));
            }

            private void Capture(string? message)
            {
                if (message != null && message.Contains(_filter, StringComparison.Ordinal))
                {
                    Messages.Enqueue(message);
                }
            }
        }
    }
}
