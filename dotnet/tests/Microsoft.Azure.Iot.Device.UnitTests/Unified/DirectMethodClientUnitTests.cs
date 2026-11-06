// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Models;
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
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
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
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
        public void DirectMethodInvokedAsync_RejectsASecondHandler()
        {
            MockFeatureConnectionClient connection = new()
            {
                CurrentConnectionContext = MockFeatureConnectionClient.CreateConnectionContext(IotHubConnectionType.Mqttv3, DeviceId),
            };
            using DirectMethodClient directMethodClient = new(connection);
            Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> handler =
                _ => Task.FromResult(new DirectMethodResponse() { Status = 200 });

            directMethodClient.DirectMethodInvokedAsync += handler;

            Assert.Throws<InvalidOperationException>(() => directMethodClient.DirectMethodInvokedAsync += handler);

            directMethodClient.DirectMethodInvokedAsync -= handler;
            directMethodClient.DirectMethodInvokedAsync += handler;
        }
    }
}
