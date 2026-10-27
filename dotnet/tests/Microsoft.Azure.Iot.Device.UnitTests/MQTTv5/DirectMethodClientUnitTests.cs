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
