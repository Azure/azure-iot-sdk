using Google.Protobuf;
using Microsoft.Azure.Devices.Client.DirectMethods;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    public class DirectMethodClientUnitTests
    {
        [Fact]
        public async Task AzureEventGridDirectMethodResultWrapsApplicationPayload()
        {
            const string deviceId = "device-1";
            Guid requestId = Guid.Parse("00112233-4455-6677-8899-aabbccddeeff");
            byte[] readyId = Guid.Parse("10213243-5465-7687-98a9-bacbdcedfe0f").ToByteArray();
            byte[] responseBody = Encoding.UTF8.GetBytes("Some response payload");
            var publishedMessages = new List<MqttPublish>();
            var connection = new MockConnectionClient();
            connection.SetCurrentConnectionContext(new ConnectionContext
            {
                DeviceId = deviceId,
                IsAzureEventGrid = true,
                IotHubHostName = "example.test"
            });
            connection.OnPublishAttempt += publish =>
            {
                publishedMessages.Add(publish);
                return Task.FromResult(new MqttPublishAck { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            using var client = new DirectMethodClient(connection);
            client.DirectMethodProbeReceivedAsync += _ => Task.FromResult(new ProbeAck
            {
                Ready = new Ready { ReadyId = ByteString.CopyFrom(readyId) }
            });
            client.DirectMethodInvokedAsync += _ => Task.FromResult(new DirectMethodResponse
            {
                Status = 200,
                Payload = responseBody
            });

            await connection.SimulateReceivePublishAsync(CreateInboundPublish(
                deviceId,
                requestId,
                "probe:1",
                new Probe
                {
                    MethodName = "reboot",
                    ResponseTimeoutSeconds = 30
                }.ToByteArray()));
            await connection.SimulateReceivePublishAsync(CreateInboundPublish(
                deviceId,
                requestId,
                "exec:1",
                new Exec
                {
                    ReadyId = ByteString.CopyFrom(readyId),
                    Params = ByteString.CopyFromUtf8("{\"delaySeconds\":0}")
                }.ToByteArray()));

            Assert.Equal(2, publishedMessages.Count);
            MqttPublish resultPublish = publishedMessages[1];
            Assert.Equal($"ih/{deviceId}/srv/methods", resultPublish.Topic);
            Assert.Equal("result:1", Assert.Single(resultPublish.UserProperties).ReadValueAsString());
            Result result = Result.Parser.ParseFrom(resultPublish.Payload);
            Assert.Equal(200, result.Status);
            Assert.Equal(responseBody, result.Body.ToByteArray());
        }

        private static MqttPublish CreateInboundPublish(
            string deviceId,
            Guid requestId,
            string messageType,
            byte[] payload)
        {
            var publish = new MqttPublish
            {
                Topic = $"ih/{deviceId}/dev/methods",
                Payload = payload,
                CorrelationData = requestId.ToByteArray(bigEndian: true),
                MessageExpiryInterval = 30,
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce
            };
            publish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(messageType)));
            return publish;
        }
    }
}
