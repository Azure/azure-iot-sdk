// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    /// <summary>
    /// An <see cref="AbstractConnectionClient"/> with the simplest possible hub-specific behavior, so that tests of the
    /// logic in the abstract client itself are not entangled with any particular generation's device presence flow.
    /// </summary>
    internal sealed class TestConnectionClient : AbstractConnectionClient, IDisposable
    {
        public TestConnectionClient(ConnectionClientOptions? options = null) : base(options)
        {
        }

        /// <summary>The CONNECTs that this client was asked to patch before they were sent to IoT hub.</summary>
        public List<MqttConnect> PatchedConnects { get; } = new();

        public override MqttConnect MqttConnectOverride(MqttConnect connect)
        {
            lock (PatchedConnects)
            {
                PatchedConnects.Add(connect);
            }

            return connect;
        }

        public override async Task HandleConnectedToHubAsync(MqttClientConnectedEventArgs args)
        {
            // This client has no birth message or topics to subscribe to, so it is present as soon as it is connected.
            await RaiseDevicePresenceFlowCompletedAsync(new DevicePresenceFlowCompletedArgs() { IsSuccess = true });
        }
    }
}
