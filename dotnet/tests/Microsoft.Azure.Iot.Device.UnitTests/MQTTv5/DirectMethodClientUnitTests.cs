// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Models;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using Xunit;

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
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()));

            MqttPublish probeAckPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal(OutgoingMethodsTopic, probeAckPublish.Topic);
            Assert.Equal("probe-ack:1", GetType(probeAckPublish));

            ProbeAck sentProbeAck = ProbeAck.Parser.ParseFrom(probeAckPublish.Payload);
            Assert.NotNull(sentProbeAck.Ready);
            ByteString readyId = sentProbeAck.Ready.ReadyId;

            // 2. Deliver the exec that references the ready id, and assert the result is published.
            var exec = new Exec() { ReadyId = readyId, Params = ByteString.CopyFromUtf8("{\"delaySeconds\":0}") };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

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
        [InlineData(false, false)]
        [InlineData(true, false)]
        [InlineData(false, true)]
        public async Task Probe_WithoutBothHandlers_AcknowledgesAndRejectsAsMethodNotFound(bool registerProbeHandler, bool registerMethodHandler)
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
            MockMqttPublishReceivedEventArgs args = await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()));

            Assert.Equal(1, args.AcknowledgeCount);
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
        public async Task DuplicateProbe_ResendsOriginalReadyId()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            bool methodInvoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
            {
                probeHandlerInvocations++;
                return Task.FromResult(DirectMethodProbeAck.Accepted());
            };
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));

            Assert.Equal(2, connection.PublishedMessages.Count);
            ByteString firstReadyId = ProbeAck.Parser.ParseFrom(connection.PublishedMessages[0].Payload).Ready.ReadyId;
            ByteString secondReadyId = ProbeAck.Parser.ParseFrom(connection.PublishedMessages[1].Payload).Ready.ReadyId;
            Assert.Equal(firstReadyId, secondReadyId);
            Assert.Equal(correlationData, connection.PublishedMessages[1].CorrelationData);
            Assert.Equal(1, probeHandlerInvocations);

            var exec = new Exec() { ReadyId = firstReadyId };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.True(methodInvoked);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[2]));
        }

        [Fact]
        public async Task DuplicateProbe_WhileHandlerInFlight_SharesDecision()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            TaskCompletionSource probeHandlerEntered = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseProbeHandler = new(TaskCreationOptions.RunContinuationsAsynchronously);
            bool methodInvoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += async (_) =>
            {
                Interlocked.Increment(ref probeHandlerInvocations);
                probeHandlerEntered.TrySetResult();
                await releaseProbeHandler.Task;
                return DirectMethodProbeAck.Accepted();
            };
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            Task firstProbe = connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));
            await probeHandlerEntered.Task;
            Task secondProbe = connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));
            Assert.False(secondProbe.IsCompleted);

            releaseProbeHandler.SetResult();
            await Task.WhenAll(firstProbe, secondProbe);

            Assert.Equal(1, probeHandlerInvocations);
            Assert.Equal(2, connection.PublishedMessages.Count);
            ByteString firstReadyId = ProbeAck.Parser.ParseFrom(connection.PublishedMessages[0].Payload).Ready.ReadyId;
            ByteString secondReadyId = ProbeAck.Parser.ParseFrom(connection.PublishedMessages[1].Payload).Ready.ReadyId;
            Assert.Equal(firstReadyId, secondReadyId);

            var exec = new Exec() { ReadyId = firstReadyId };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.True(methodInvoked);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[2]));
        }

        [Fact]
        public async Task DuplicateProbe_AfterExecConsumedRequest_DoesNotRestoreReadyId()
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

            // Hold the PUBACK of the first probe ack so that the original probe is still in flight while the exec and duplicate probe arrive.
            TaskCompletionSource<MqttPublishAck> firstProbeAckPuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource firstProbeAckPublished = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int probeAcksPublished = 0;
            connection.OnPublish = (publish) =>
            {
                if (GetType(publish) == "probe-ack:1" && Interlocked.Increment(ref probeAcksPublished) == 1)
                {
                    firstProbeAckPublished.TrySetResult();
                    return firstProbeAckPuback.Task;
                }

                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            Task firstProbe = connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));
            await firstProbeAckPublished.Task;
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;

            byte[] execPayload = new Exec() { ReadyId = readyId }.ToByteArray();
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, execPayload));
            Assert.Equal(1, methodInvocations);

            // A duplicate probe that arrives after the exec must not make the consumed ready Id valid again.
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));

            firstProbeAckPuback.SetResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            await firstProbe;

            // A redelivered exec must not invoke the method again.
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, execPayload));

            Assert.Equal(1, probeHandlerInvocations);
            Assert.Equal(1, methodInvocations);
            Assert.Equal(1, probeAcksPublished);
            Assert.Equal(2, connection.PublishedMessages.Count);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[1]));
        }

        [Fact]
        public async Task DuplicateProbe_WithDifferentMethodName_IsIgnored()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            string? invokedMethodName = null;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
            {
                probeHandlerInvocations++;
                return Task.FromResult(DirectMethodProbeAck.Accepted());
            };
            directMethodClient.DirectMethodInvokedAsync += (args) =>
            {
                invokedMethodName = args.MethodName;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray()));
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, new Probe() { MethodName = "factoryReset", ResponseTimeoutSeconds = 30 }.ToByteArray()));

            Assert.Equal(1, probeHandlerInvocations);
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;

            var exec = new Exec() { ReadyId = readyId };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.Equal("reboot", invokedMethodName);
            Assert.Equal(2, connection.PublishedMessages.Count);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[1]));
        }

        [Fact]
        public async Task ProbeThatTimesOut_DoesNotCacheRequest()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            bool methodInvoked = false;
            directMethodClient.DirectMethodProbeReceivedAsync += async (_) =>
            {
                if (++probeHandlerInvocations == 1)
                {
                    await Task.Delay(TimeSpan.FromMilliseconds(2100));
                }

                return DirectMethodProbeAck.Accepted();
            };
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 1));
            Assert.Empty(connection.PublishedMessages);

            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));

            Assert.Equal(2, probeHandlerInvocations);
            MqttPublish probeAckPublish = Assert.Single(connection.PublishedMessages);
            ByteString readyId = ProbeAck.Parser.ParseFrom(probeAckPublish.Payload).Ready.ReadyId;

            var exec = new Exec() { ReadyId = readyId };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.True(methodInvoked);
            Assert.Equal(2, connection.PublishedMessages.Count);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[1]));
        }

        [Fact]
        public async Task DuplicateProbe_AfterExec_ExtendsConsumedRequestDeadline()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
            {
                Interlocked.Increment(ref probeHandlerInvocations);
                return Task.FromResult(DirectMethodProbeAck.Accepted());
            };
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 1));
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, new Exec() { ReadyId = readyId }.ToByteArray()));

            // A duplicate with a longer expiry arrives after the exec, so the consumed request must outlive the original probe's expiry.
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 30));
            await Task.Delay(TimeSpan.FromMilliseconds(1500), TestContext.Current.CancellationToken);
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 30));

            Assert.Equal(1, probeHandlerInvocations);
            Assert.Equal(2, connection.PublishedMessages.Count);
        }

        [Fact]
        public async Task DuplicateProbe_StillAcknowledgingWhenConsumedRequestExpires_IsIgnored()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
            {
                Interlocked.Increment(ref probeHandlerInvocations);
                return Task.FromResult(DirectMethodProbeAck.Accepted());
            };
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 1));
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, new Exec() { ReadyId = readyId }.ToByteArray()));

            // Hold the duplicate's acknowledgment until the consumed request has expired and been pruned.
            TaskCompletionSource releaseAcknowledgment = new(TaskCreationOptions.RunContinuationsAsynchronously);
            MockMqttPublishReceivedEventArgs duplicateArgs = new()
            {
                Publish = CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 1),
                AcknowledgeResult = releaseAcknowledgment.Task,
            };
            Task duplicateProbe = connection.SimulateReceiveAsync(duplicateArgs);

            await Task.Delay(TimeSpan.FromMilliseconds(1500), TestContext.Current.CancellationToken);
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", Guid.NewGuid().ToByteArray(), new Exec() { ReadyId = readyId }.ToByteArray()));

            releaseAcknowledgment.SetResult();
            await duplicateProbe;

            Assert.Equal(1, probeHandlerInvocations);
            Assert.Equal(2, connection.PublishedMessages.Count);
        }

        [Fact]
        public async Task DuplicateProbe_OutlivingTimedOutOwner_KeepsSharingItsDecision()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            TaskCompletionSource probeHandlerEntered = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseProbeHandler = new(TaskCreationOptions.RunContinuationsAsynchronously);
            directMethodClient.DirectMethodProbeReceivedAsync += async (_) =>
            {
                Interlocked.Increment(ref probeHandlerInvocations);
                probeHandlerEntered.TrySetResult();
                await releaseProbeHandler.Task;
                return DirectMethodProbeAck.Accepted();
            };
            bool methodInvoked = false;
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            Task owner = connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 1));
            await probeHandlerEntered.Task;
            Task duplicate = connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));

            // The owner gives up at its own expiry, but the live duplicate keeps the request registered.
            Assert.Same(owner, await Task.WhenAny(owner, Task.Delay(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken)));
            Task thirdProbe = connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));

            releaseProbeHandler.SetResult();
            await Task.WhenAll(duplicate, thirdProbe);

            Assert.Equal(1, probeHandlerInvocations);
            Assert.Equal(2, connection.PublishedMessages.Count);
            ProbeAck firstAck = ProbeAck.Parser.ParseFrom(connection.PublishedMessages[0].Payload);
            ProbeAck secondAck = ProbeAck.Parser.ParseFrom(connection.PublishedMessages[1].Payload);
            Assert.Equal(ProbeAck.ResultOneofCase.Ready, firstAck.ResultCase);
            Assert.Equal(firstAck.Ready.ReadyId, secondAck.Ready.ReadyId);

            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, new Exec() { ReadyId = firstAck.Ready.ReadyId }.ToByteArray()));
            Assert.True(methodInvoked);
        }

        [Fact]
        public async Task DuplicateProbe_WhenHandlerNeverCompletes_StopsWaitingAtItsExpiry()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            TaskCompletionSource<DirectMethodProbeAck> neverCompletes = new();
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
                Interlocked.Increment(ref probeHandlerInvocations) == 1
                    ? neverCompletes.Task
                    : Task.FromResult(DirectMethodProbeAck.Accepted());
            bool methodInvoked = false;
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                methodInvoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            byte[] probePayload = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray();

            Task probes = Task.WhenAll(
                connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 1)),
                connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload, messageExpiryInterval: 2)));
            Assert.Same(probes, await Task.WhenAny(probes, Task.Delay(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken)));
            Assert.Empty(connection.PublishedMessages);

            // Once every live probe has expired, a retry asks the application again.
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probePayload));

            Assert.Equal(2, probeHandlerInvocations);
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, new Exec() { ReadyId = readyId }.ToByteArray()));
            Assert.True(methodInvoked);
        }

        [Fact]
        public async Task DuplicateProbe_WithDifferentResponseTimeout_IsIgnored()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            int probeHandlerInvocations = 0;
            directMethodClient.DirectMethodProbeReceivedAsync += (_) =>
            {
                probeHandlerInvocations++;
                return Task.FromResult(DirectMethodProbeAck.Accepted());
            };
            directMethodClient.DirectMethodInvokedAsync += (_) => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray()));
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 5 }.ToByteArray()));

            Assert.Equal(1, probeHandlerInvocations);
            Assert.Single(connection.PublishedMessages);
        }

        [Fact]
        public async Task Exec_WithWrongReadyId_DoesNotConsumeRequest()
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
                methodInvocations++;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            byte[] correlationData = Guid.NewGuid().ToByteArray();
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 }.ToByteArray()));
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;

            var wrongExec = new Exec() { ReadyId = ByteString.CopyFrom(Guid.NewGuid().ToByteArray()) };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, wrongExec.ToByteArray()));
            Assert.Equal(0, methodInvocations);

            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, new Exec() { ReadyId = readyId }.ToByteArray()));
            Assert.Equal(1, methodInvocations);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[1]));
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
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", Guid.NewGuid().ToByteArray(), exec.ToByteArray()));

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

            await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "ih/someDeviceId/dev/twin",
                Payload = Encoding.UTF8.GetBytes("irrelevant"),
            });

            Assert.False(invoked);
            Assert.Empty(connection.PublishedMessages);
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
            var probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()));
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;

            using var listener = new CapturingTraceListener(requestId.ToString());
            Trace.Listeners.Add(listener);
            try
            {
                // Field 1 (ready_id) declares 5 bytes of data, but none follow, so this is a truncated protobuf message.
                byte[] malformedExecPayload = [0x0A, 0x05];
                await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, malformedExecPayload));
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
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.True(methodInvoked);
            Assert.Equal(2, connection.PublishedMessages.Count);
            Assert.Equal("result:1", GetType(connection.PublishedMessages[1]));
        }

        [Fact]
        public async Task Exec_CancelsHandlerTokenWhenResponseTimeoutElapses()
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
            var probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 1 };
            await connection.SimulateReceiveAsync(CreateInboundMethodPublish("probe:1", correlationData, probe.ToByteArray()));
            ByteString readyId = ProbeAck.Parser.ParseFrom(Assert.Single(connection.PublishedMessages).Payload).Ready.ReadyId;

            var exec = new Exec() { ReadyId = readyId };
            Task receiveTask = connection.SimulateReceiveAsync(CreateInboundMethodPublish("exec:1", correlationData, exec.ToByteArray(), messageExpiryInterval: 1));

            Assert.Same(receiveTask, await Task.WhenAny(receiveTask, Task.Delay(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken)));
            await receiveTask;

            Assert.True(handlerToken.IsCancellationRequested);
            Assert.Single(connection.PublishedMessages);
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
