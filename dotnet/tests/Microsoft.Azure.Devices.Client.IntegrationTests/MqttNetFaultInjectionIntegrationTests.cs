using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter.Session;
using System;
using System.Collections.Generic;
using System.Text;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests 
{
    // This integration test suite has the MQTTnet client adapter connect to a faultable MQTT broker (sourced from the AIO SDK repo here:https://github.com/Azure/iot-operations-sdks/tree/main/eng/test/faultablemqttbroker/src/Azure.Iot.Operations.FaultableMqttBroker).
    //
    // This allows us to test scenarios where the MQTT connection drops unexpectedly.
    //
    // Note that these scenarios cannot be tested against IoT hub directly as AEG Hub does not support fault injection messages and not all versions of classic hub do either.
    // Additionally, we cannot use the top level ConnectionClient to test these scenarios as that client deliberately does not allow the injection of user properties into
    // MQTT-level packets.
    public class MqttNetFaultInjectionIntegrationTests
    {
        private MqttConnect CreateConnectPacket()
        {
            return new()
            {
                HostName = "localhost",
                ProtocolVersion = MqttProtocolVersion.V500, //TODO Do we want to try both MQTTv5 and mqttv3 brokers? mqttv3 doesn't support user properties, so this is difficult
                TcpPort = 1884,
                CleanSession = true,
                CleanStart = true,
                ClientId = Guid.NewGuid().ToString(),
            };
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestSessionClientHandlesFailedConnackDuringConnect()
        {
            MqttSessionClient mqttClient = new();

            MqttClientDisconnectReason expectedReason = MqttClientDisconnectReason.ServerBusy;

            MqttConnect connectPacket = CreateConnectPacket();
            connectPacket.AddUserProperty(FaultInjectionTestConstants.rejectConnectFaultName, "" + ((int)expectedReason));
            connectPacket.AddUserProperty(FaultInjectionTestConstants.faultRequestIdName, Guid.NewGuid().ToString());

            // The first connection attempt should fail, but the session client's retry policy should make it
            // connect again. The broker should accept the second connection attempt.
            var connAck = await mqttClient.ConnectAsync(connectPacket);
            Assert.Equal(MqttConnectResultCode.Success, connAck.ResultCode);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestSessionClientHandlesDisconnectWhileIdle()
        {
            MqttSessionClient mqttClient = new();
            MqttConnect connectPacket = CreateConnectPacket();
            await mqttClient.ConnectAsync(connectPacket);

            TaskCompletionSource<MqttClientDisconnectedEventArgs> faultWasInjectedTcs = new();
            mqttClient.DisconnectedAsync += (args) =>
            {
                faultWasInjectedTcs.TrySetResult(args);
                return Task.CompletedTask;
            };

            MqttClientDisconnectReason expectedReason = MqttClientDisconnectReason.ServerBusy;
            byte[] expectedPayload = Guid.NewGuid().ToByteArray();

            // This fault injection publish will be ack'd as normal, but will tell the broker
            // to kill the connection 1 second after receiving the publish
            MqttPublish faultMessage = new MqttPublish()
            {
                PayloadAsArraySegment = expectedPayload,
                Topic = "some/irrelevant/topic",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            faultMessage.AddUserProperty(FaultInjectionTestConstants.disconnectFaultName, "" + ((int)expectedReason));
            faultMessage.AddUserProperty(FaultInjectionTestConstants.disconnectFaultDelayName, "1");
            faultMessage.AddUserProperty(FaultInjectionTestConstants.faultRequestIdName, Guid.NewGuid().ToString());

            var result = await mqttClient.PublishAsync(faultMessage).WaitAsync(TimeSpan.FromMinutes(1));
            Assert.Equal(MqttClientPublishReasonCode.NoMatchingSubscribers, result.ReasonCode);

            // Wait until the fault injection happens or until a timeout
            var faultDetails = await faultWasInjectedTcs.Task.WaitAsync(TimeSpan.FromSeconds(30));
            Assert.Equal(expectedReason, faultDetails.Reason);

            // The session client should handle the fault and reconnect either prior to this publish or after this publish
            // is initiated. In either case, the publish should be sent successfully
            var subsequentPublish = new MqttPublish()
            {
                Topic = "some/irrelevant/topic",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            result = await mqttClient.PublishAsync(subsequentPublish);
            Assert.Equal(MqttClientPublishReasonCode.NoMatchingSubscribers, result.ReasonCode);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestSessionClientHandlesDisconnectDuringPublish()
        {
            MqttSessionClient mqttClient = new();
            MqttConnect connectPacket = CreateConnectPacket();
            await mqttClient.ConnectAsync(connectPacket);


            TaskCompletionSource<MqttClientDisconnectedEventArgs> faultWasInjectedTcs = new();
            mqttClient.DisconnectedAsync += (args) =>
            {
                faultWasInjectedTcs.TrySetResult(args);
                return Task.CompletedTask;
            };

            MqttClientDisconnectReason expectedReason = MqttClientDisconnectReason.AdministrativeAction;
            byte[] expectedPayload = Guid.NewGuid().ToByteArray();

            MqttPublish faultMessage = new()
            {
                PayloadAsArraySegment = expectedPayload,
                Topic = "some/irrelevant/topic",
                QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
            };

            faultMessage.AddUserProperty(FaultInjectionTestConstants.disconnectFaultName, "" + ((int)expectedReason));
            faultMessage.AddUserProperty(FaultInjectionTestConstants.faultRequestIdName, Guid.NewGuid().ToString());

            var result = await mqttClient.PublishAsync(faultMessage).WaitAsync(TimeSpan.FromMinutes(1));

            Assert.Equal(expectedReason, (await faultWasInjectedTcs.Task.WaitAsync(TimeSpan.FromMinutes(1))).Reason);
            Assert.Equal(MqttClientPublishReasonCode.NoMatchingSubscribers, result.ReasonCode);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestSessionClientHandlesDisconnectDuringSubscribe()
        {
            MqttSessionClient mqttClient = new();
            MqttConnect connectPacket = CreateConnectPacket();
            await mqttClient.ConnectAsync(connectPacket);

            TaskCompletionSource<MqttClientDisconnectedEventArgs> faultWasInjectedTcs = new();
            mqttClient.DisconnectedAsync += (args) =>
            {
                faultWasInjectedTcs.TrySetResult(args);
                return Task.CompletedTask;
            };

            MqttClientDisconnectReason expectedReason = MqttClientDisconnectReason.AdministrativeAction;
            string expectedTopic = "myTopic/" + Guid.NewGuid().ToString();
            var subscribeOptions = new MqttSubscribe(expectedTopic, MqttQualityOfServiceLevel.AtLeastOnce);
            subscribeOptions.AddUserProperty(FaultInjectionTestConstants.disconnectFaultName, "" + ((int)expectedReason));
            subscribeOptions.AddUserProperty(FaultInjectionTestConstants.faultRequestIdName, Guid.NewGuid().ToString());

            MqttSubscribeAck subscribeResult = await mqttClient.SubscribeAsync(subscribeOptions).WaitAsync(TimeSpan.FromMinutes(1));

            Assert.Equal(expectedReason, (await faultWasInjectedTcs.Task.WaitAsync(TimeSpan.FromMinutes(1))).Reason);
            Assert.Single(subscribeResult.Items);
        }

        [Fact(Timeout = Setup.TestTimeoutMilliseconds)]
        public async Task TestSessionClientHandlesDisconnectDuringUnsubscribe()
        {
            MqttSessionClient mqttClient = new();
            MqttConnect connectPacket = CreateConnectPacket();
            await mqttClient.ConnectAsync(connectPacket);

            TaskCompletionSource<MqttClientDisconnectedEventArgs> faultWasInjectedTcs = new();
            mqttClient.DisconnectedAsync += (args) =>
            {
                faultWasInjectedTcs.TrySetResult(args);
                return Task.CompletedTask;
            };

            string expectedTopic = "myTopic/" + Guid.NewGuid().ToString();
            await mqttClient.SubscribeAsync(new MqttSubscribe(expectedTopic, MqttQualityOfServiceLevel.AtLeastOnce));

            MqttClientDisconnectReason expectedReason = MqttClientDisconnectReason.ConnectionRateExceeded;
            var unsubscribeOptions = new MqttUnsubscribe(expectedTopic);
            unsubscribeOptions.AddUserProperty(FaultInjectionTestConstants.disconnectFaultName, "" + ((int)expectedReason));
            unsubscribeOptions.AddUserProperty(FaultInjectionTestConstants.faultRequestIdName, Guid.NewGuid().ToString());
            MqttUnsubscribeAck unsubscribeResult =
                await mqttClient.UnsubscribeAsync(unsubscribeOptions).WaitAsync(TimeSpan.FromMinutes(1));

            Assert.Equal(expectedReason, (await faultWasInjectedTcs.Task.WaitAsync(TimeSpan.FromMinutes(1))).Reason);
            Assert.Single(unsubscribeResult.Items);
        }
    }
}
