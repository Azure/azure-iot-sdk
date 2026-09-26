// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Microsoft.Azure.Iot.Device.Unified.DirectMethods;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class DirectMethodClientUnitTests
    {
        private const string DeviceId = "someDeviceId";

        [Fact]
        public async Task Classic_InvokesHandlerAndPublishesResponse()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            byte[] responseBody = Encoding.UTF8.GetBytes("{\"result\":\"ok\"}");
            DirectMethodRequestReceivedEventArgs? invokedArgs = null;

            directMethodClient.DirectMethodInvokedAsync += (args) =>
            {
                invokedArgs = args;
                return Task.FromResult(new DirectMethodResponse() { Status = 200, Payload = responseBody });
            };

            await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = "$iothub/methods/POST/reboot/?$rid=42",
                Payload = Encoding.UTF8.GetBytes("{\"delaySeconds\":0}"),
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtMostOnce,
            });

            Assert.NotNull(invokedArgs);
            Assert.Equal("reboot", invokedArgs!.MethodName);
            Assert.Equal("{\"delaySeconds\":0}", Encoding.UTF8.GetString(invokedArgs.Payload!));

            MqttPublish responsePublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal("$iothub/methods/res/200/?$rid=42", responsePublish.Topic);
            Assert.Equal(responseBody, responsePublish.Payload);
        }

        [Fact]
        public async Task Classic_IgnoresUnrelatedTopics()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.Classic, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            bool invoked = false;
            directMethodClient.DirectMethodInvokedAsync += (_) =>
            {
                invoked = true;
                return Task.FromResult(new DirectMethodResponse() { Status = 200 });
            };

            await connection.SimulateReceiveAsync(new MqttPublish()
            {
                Topic = $"devices/{DeviceId}/messages/devicebound/irrelevant",
                Payload = Encoding.UTF8.GetBytes("irrelevant"),
            });

            Assert.False(invoked);
            Assert.Empty(connection.PublishedMessages);
        }

        [Fact]
        public async Task MqttV5_AutoAcceptsProbeThenInvokesHandlerAndPublishesResult()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(ConnectionProfile.MqttV5, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);

            byte[] responseBody = Encoding.UTF8.GetBytes("done");
            directMethodClient.DirectMethodInvokedAsync += (_) =>
                Task.FromResult(new DirectMethodResponse() { Status = 200, Payload = responseBody });

            byte[] correlationData = Guid.NewGuid().ToByteArray();

            // The unified client auto-accepts any probe on an MQTT 5 hub since classic hubs have no probe concept.
            var probe = new Probe() { MethodName = "reboot", ResponseTimeoutSeconds = 30 };
            await connection.SimulateReceiveAsync(CreateMqttv5MethodPublish("probe:1", correlationData, probe.ToByteArray()));

            MqttPublish probeAckPublish = Assert.Single(connection.PublishedMessages);
            Assert.Equal($"ih/{DeviceId}/srv/methods", probeAckPublish.Topic);
            Assert.Equal("probe-ack:1", GetType(probeAckPublish));

            ProbeAck sentProbeAck = ProbeAck.Parser.ParseFrom(probeAckPublish.Payload);
            ByteString readyId = sentProbeAck.Ready.ReadyId;

            var exec = new Exec() { ReadyId = readyId, Params = ByteString.CopyFromUtf8("{}") };
            await connection.SimulateReceiveAsync(CreateMqttv5MethodPublish("exec:1", correlationData, exec.ToByteArray()));

            Assert.Equal(2, connection.PublishedMessages.Count);
            MqttPublish resultPublish = connection.PublishedMessages[1];
            Assert.Equal("result:1", GetType(resultPublish));
            Result result = Result.Parser.ParseFrom(resultPublish.Payload);
            Assert.Equal(200, result.Status);
            Assert.Equal(responseBody, result.Body.ToByteArray());
        }

        private static MqttPublish CreateMqttv5MethodPublish(string type, byte[] correlationData, byte[] payload)
        {
            var publish = new MqttPublish()
            {
                Topic = $"ih/{DeviceId}/dev/methods",
                Payload = payload,
                CorrelationData = correlationData,
                MessageExpiryInterval = 30,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(type)));
            return publish;
        }

        private static string GetType(MqttPublish publish)
        {
            return Assert.Single(publish.UserProperties, p => p.Name == "type").ReadValueAsString();
        }
    }
}
