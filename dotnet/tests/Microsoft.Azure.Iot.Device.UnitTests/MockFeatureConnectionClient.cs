// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Provisioning.Models;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// A lightweight fake connection client used to unit test the various feature clients (telemetry, twin, direct
    /// methods) in isolation from the real connection/MQTT layers.
    /// </summary>
    /// <remarks>
    /// It implements both the MQTTv5 and unified <c>IConnectionClient</c> interfaces (which are structurally identical) so
    /// that a single instance can be handed to feature clients from either namespace. Tests control what the "service"
    /// does by inspecting <see cref="PublishedMessages"/>, wiring <see cref="OnPublish"/>, and injecting inbound traffic
    /// with <see cref="SimulateReceiveAsync"/>.
    /// </remarks>
    internal sealed class MockFeatureConnectionClient
        : global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        /// <summary>The context returned to feature clients. Null simulates a client that has never connected.</summary>
        public ConnectionContext? CurrentConnectionContext { get; set; }

        /// <summary>Every publish, subscribe and unsubscribe the feature clients sent, in order.</summary>
        public List<MqttPublish> PublishedMessages { get; } = new();

        public List<MqttSubscribe> SubscribedMessages { get; } = new();

        public List<MqttUnsubscribe> UnsubscribedMessages { get; } = new();

        /// <summary>Optional override for how the "service" answers a publish. Defaults to a successful PUBACK.</summary>
        public Func<MqttPublish, Task<MqttPublishAck>>? OnPublish { get; set; }

        public bool IsDisposed { get; private set; }

        /// <summary>
        /// Whether IoT hub is ready for twin requests on the current connection. Only effective while
        /// <see cref="CurrentConnectionContext"/> is set.
        /// </summary>
        public bool IsConnectionReady { get; set; } = true;

        public event Action? ConnectionLost;

        public event Action? ConnectionReady;

        bool global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.IsConnectionReady
            => IsConnectionReady && CurrentConnectionContext != null;

        event Action? global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ConnectionLost
        {
            add => ConnectionLost += value;
            remove => ConnectionLost -= value;
        }

        event Action? global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ConnectionReady
        {
            add => ConnectionReady += value;
            remove => ConnectionReady -= value;
        }

        /// <summary>Identifies the current connection. Incremented by <see cref="SimulateConnectionReady"/>.</summary>
        public long HubConnectionId { get; set; }

        /// <summary>The twin that this "connection client" got on the current connection. Null by default.</summary>
        public global::Microsoft.Azure.Iot.Device.Unified.Connection.ConnectTwinSnapshot? LatestConnectTwinSnapshot { get; set; }

        long global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.HubConnectionId => HubConnectionId;

        global::Microsoft.Azure.Iot.Device.Unified.Connection.ConnectTwinSnapshot? global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.LatestConnectTwinSnapshot
            => LatestConnectTwinSnapshot;

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        /// <summary>Snapshot of <see cref="PublishedMessages"/>, safe to take while feature clients publish from other threads.</summary>
        public List<MqttPublish> GetPublishedMessages()
        {
            lock (PublishedMessages)
            {
                return new List<MqttPublish>(PublishedMessages);
            }
        }

        /// <summary>Simulate losing the connection to IoT hub.</summary>
        public void SimulateConnectionLost()
        {
            IsConnectionReady = false;
            LatestConnectTwinSnapshot = null;
            ConnectionLost?.Invoke();
        }

        /// <summary>
        /// Simulate IoT hub completing the device presence flow on a new connection, during which the "connection client"
        /// got the given twin (if any). The snapshot is used as given, so a test can give one with the ID of a previous
        /// connection; use <see cref="CreateConnectTwinSnapshot"/> to create one for the new connection.
        /// </summary>
        public void SimulateConnectionReady(global::Microsoft.Azure.Iot.Device.Unified.Connection.ConnectTwinSnapshot? snapshot = null)
        {
            HubConnectionId++;
            LatestConnectTwinSnapshot = snapshot;
            IsConnectionReady = true;
            ConnectionReady?.Invoke();
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            lock (PublishedMessages)
            {
                PublishedMessages.Add(publish);
            }

            if (OnPublish != null)
            {
                return await OnPublish.Invoke(publish);
            }

            return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
        }

        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            SubscribedMessages.Add(subscribe);
            return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            UnsubscribedMessages.Add(unsubscribe);
            return Task.FromResult(MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe));
        }

        /// <summary>Deliver an inbound publish to whatever feature clients have subscribed to this connection.</summary>
        public Task SimulateReceiveAsync(MqttPublish publish)
            => SimulateReceiveAsync(new MockMqttPublishReceivedEventArgs() { Publish = publish });

        public async Task SimulateReceiveAsync(MqttPublishReceivedEventArgs args)
        {
            if (PublishReceivedAsync != null)
            {
                await PublishReceivedAsync.Invoke(args);
            }
        }

        public void Dispose()
        {
            IsDisposed = true;
        }

        /// <summary>Create the twin that the "connection client" got on the connection that the next <see cref="SimulateConnectionReady"/> simulates.</summary>
        public global::Microsoft.Azure.Iot.Device.Unified.Connection.ConnectTwinSnapshot CreateConnectTwinSnapshot(ulong desiredVersion, string desired, ulong reportedVersion, string reported)
        {
            return new(
                HubConnectionId + 1,
                System.Text.Json.Nodes.JsonNode.Parse(desired)!.AsObject(),
                desiredVersion,
                System.Text.Json.Nodes.JsonNode.Parse(reported)!.AsObject(),
                reportedVersion);
        }

        /// <summary>Build a connection context for the given hub generation.</summary>
        public static ConnectionContext CreateConnectionContext(
            IotHubConnectionType connectionProfile,
            string deviceId = "someDeviceId",
            string iotHubHostName = "someHostName")
        {
#pragma warning disable SYSLIB0026 // Mock certificate; no need to load a real one.
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2(), null),
                DeviceId = deviceId,
                IotHubHostName = iotHubHostName,
                ConnectionProfile = connectionProfile,
            };
#pragma warning restore SYSLIB0026
        }
    }
}
