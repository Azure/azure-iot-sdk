// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class MqttConnectionManagerUnitTests
    {
        [Fact]
        public async Task ServerUnavailableConnackRetriesWithFreshConnectPacket()
        {
            using MockConnectionMqttClient mqttClient = new();
            int serverAttempt = 0;
            mqttClient.OnConnect = _ => Task.FromResult(new MqttConnectAck
            {
                ResultCode = Interlocked.Increment(ref serverAttempt) == 1
                    ? MqttConnectReasonCode.ServerUnavailable
                    : MqttConnectReasonCode.Success,
            });

            using MqttConnectionManager connection = new(
                mqttClient,
                TimeSpan.FromSeconds(5),
                new ImmediateRetryPolicy(maxAttempts: 2));

            int connectingEventCount = 0;
            connection.ConnectingAsync += connect =>
            {
                connect.Username = $"attempt-{Interlocked.Increment(ref connectingEventCount)}";
                return Task.FromResult(connect);
            };

            MqttConnectAck result = await connection.ConnectAsync(
                new MqttConnect { HostName = "test.azure-devices.net" },
                TestContext.Current.CancellationToken);

            Assert.Equal(MqttConnectReasonCode.Success, result.ResultCode);
            Assert.Equal(2, mqttClient.ConnectAttemptCount);
            Assert.Equal(2, connectingEventCount);
        }

        private sealed class ImmediateRetryPolicy(uint maxAttempts) : IRetryPolicy
        {
            public bool ShouldRetry(
                uint currentRetryCount,
                Exception? lastException,
                out TimeSpan retryDelay)
            {
                retryDelay = TimeSpan.Zero;
                return currentRetryCount <= maxAttempts;
            }
        }
    }
}
