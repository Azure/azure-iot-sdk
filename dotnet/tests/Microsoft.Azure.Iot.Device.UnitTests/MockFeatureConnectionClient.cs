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
        : global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient,
          global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;

        /// <summary>The context returned to feature clients. Null simulates a client that has never connected.</summary>
        public ConnectionContext? CurrentConnectionContext { get; set; }

        /// <summary>Whether the connection client is connected or reconnecting. False simulates an explicit disconnect.</summary>
        public bool IsConnectionTargetSet { get; set; } = true;

        /// <summary>The connection nonce returned to feature clients. Null simulates a connection client that does not expose it.</summary>
        public Guid? ConnectionNonce { get; set; }

        Guid? global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.ConnectionNonce => ConnectionNonce;

        Guid? global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ConnectionNonce => ConnectionNonce;

        /// <summary>The versions from the latest birth ack that the connection client received, if any.</summary>
        public global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.BirthAckVersions? LatestBirthAck { get; set; }

        global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.BirthAckVersions? global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.LatestBirthAck => LatestBirthAck;

        global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.BirthAckVersions? global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.LatestBirthAck => LatestBirthAck;

        public event Action? ConnectionLost;

        event Action? global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.ConnectionLost
        {
            add => ConnectionLost += value;
            remove => ConnectionLost -= value;
        }

        event Action? global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ConnectionLost
        {
            add => ConnectionLost += value;
            remove => ConnectionLost -= value;
        }

        /// <summary>Notify feature clients that the connection to IoT hub was lost.</summary>
        public void SimulateConnectionLost()
        {
            IsClassicDevicePresenceEstablished = false;
            ConnectionLost?.Invoke();
        }

        public event Action? ClassicDevicePresenceEstablished;

        /// <summary>Whether the device presence flow completed on the current connection to an MQTTv3 IoT hub.</summary>
        public bool IsClassicDevicePresenceEstablished { get; set; }

        event Action? global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.ClassicDevicePresenceEstablished
        {
            add => ClassicDevicePresenceEstablished += value;
            remove => ClassicDevicePresenceEstablished -= value;
        }

        event Action? global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ClassicDevicePresenceEstablished
        {
            add => ClassicDevicePresenceEstablished += value;
            remove => ClassicDevicePresenceEstablished -= value;
        }

        bool global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.IsClassicDevicePresenceEstablished => IsClassicDevicePresenceEstablished;

        bool global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.IsClassicDevicePresenceEstablished => IsClassicDevicePresenceEstablished;

        /// <summary>Notify feature clients that the device presence flow completed on a new connection to an MQTTv3 IoT hub.</summary>
        public void SimulateClassicDevicePresenceEstablished(Guid? connectionNonce = null)
        {
            ConnectionNonce = connectionNonce ?? Guid.NewGuid();
            IsClassicDevicePresenceEstablished = true;
            ClassicDevicePresenceEstablished?.Invoke();
        }

        public ulong InitialDesiredPropertiesVersion { get; set; }

        public ulong InitialReportedPropertiesVersion { get; set; }

        /// <summary>Every desired properties version a feature client reported that the device now holds, in order.</summary>
        public List<ulong> ObservedDesiredPropertiesVersions { get; } = new();

        /// <summary>Every reported properties version a feature client reported that the device now holds, in order.</summary>
        public List<ulong> ObservedReportedPropertiesVersions { get; } = new();

        ulong global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.InitialDesiredPropertiesVersion => InitialDesiredPropertiesVersion;

        ulong global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.InitialReportedPropertiesVersion => InitialReportedPropertiesVersion;

        void global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.ObserveDesiredPropertiesVersion(ulong version) => ObservedDesiredPropertiesVersions.Add(version);

        void global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.ObserveReportedPropertiesVersion(ulong version) => ObservedReportedPropertiesVersions.Add(version);

        /// <summary>Every version that a feature client replaced the recorded desired properties version with, in order.</summary>
        public List<ulong> ReplacedDesiredPropertiesVersions { get; } = new();

        /// <summary>Every version that a feature client replaced the recorded reported properties version with, in order.</summary>
        public List<ulong> ReplacedReportedPropertiesVersions { get; } = new();

        void global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.ReplaceDesiredPropertiesVersion(ulong version) => ReplacedDesiredPropertiesVersions.Add(version);

        void global::Microsoft.Azure.Iot.Device.MQTTv5.Connection.IConnectionClient.ReplaceReportedPropertiesVersion(ulong version) => ReplacedReportedPropertiesVersions.Add(version);

        void global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ObserveDesiredPropertiesVersion(ulong version) => ObservedDesiredPropertiesVersions.Add(version);

        void global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ObserveReportedPropertiesVersion(ulong version) => ObservedReportedPropertiesVersions.Add(version);

        void global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ReplaceDesiredPropertiesVersion(ulong version) => ReplacedDesiredPropertiesVersions.Add(version);

        void global::Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient.ReplaceReportedPropertiesVersion(ulong version) => ReplacedReportedPropertiesVersions.Add(version);

        /// <summary>Every publish, subscribe and unsubscribe the feature clients sent, in order.</summary>
        public List<MqttPublish> PublishedMessages { get; } = new();

        /// <summary>The cancellation token passed with each publish in <see cref="PublishedMessages"/>, in the same order.</summary>
        public List<CancellationToken> PublishCancellationTokens { get; } = new();

        public List<MqttSubscribe> SubscribedMessages { get; } = new();

        public List<MqttUnsubscribe> UnsubscribedMessages { get; } = new();

        /// <summary>Optional override for how the "service" answers a publish. Defaults to a successful PUBACK.</summary>
        public Func<MqttPublish, Task<MqttPublishAck>>? OnPublish { get; set; }

        /// <summary>Optional override for how the "service" answers a subscribe. Defaults to granting every topic filter.</summary>
        public Func<MqttSubscribe, Task<MqttSubscribeAck>>? OnSubscribe { get; set; }

        /// <summary>Optional override for how the "service" answers an unsubscribe. Defaults to succeeding for every topic filter.</summary>
        public Func<MqttUnsubscribe, Task<MqttUnsubscribeAck>>? OnUnsubscribe { get; set; }

        public bool IsDisposed { get; private set; }

        public ConnectionContext? GetCurrentConnectionContext() => CurrentConnectionContext;

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            lock (PublishedMessages)
            {
                PublishedMessages.Add(publish);
                PublishCancellationTokens.Add(cancellationToken);
            }

            if (OnPublish != null)
            {
                return await OnPublish.Invoke(publish);
            }

            return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
        }

        /// <summary>
        /// Wait until at least <paramref name="count"/> publishes were sent, for publishes that feature clients send from a
        /// background thread, and return the publish at index <paramref name="count"/> - 1.
        /// </summary>
        public async Task<MqttPublish> WaitForPublishedMessagesAsync(int count)
        {
            using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(10));
            while (true)
            {
                lock (PublishedMessages)
                {
                    if (PublishedMessages.Count >= count)
                    {
                        return PublishedMessages[count - 1];
                    }
                }

                await Task.Delay(10, timeout.Token);
            }
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            SubscribedMessages.Add(subscribe);

            if (OnSubscribe != null)
            {
                return await OnSubscribe.Invoke(subscribe);
            }

            return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            UnsubscribedMessages.Add(unsubscribe);

            if (OnUnsubscribe != null)
            {
                return await OnUnsubscribe.Invoke(unsubscribe);
            }

            return MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);
        }

        /// <summary>
        /// Deliver an inbound publish to whatever feature clients have subscribed to this connection. Returns the event
        /// args that were delivered so tests can inspect how the publish was acknowledged.
        /// </summary>
        public async Task<MockMqttPublishReceivedEventArgs> SimulateReceiveAsync(MqttPublish publish)
        {
            MockMqttPublishReceivedEventArgs args = new() { Publish = publish };
            await SimulateReceiveAsync(args);
            return args;
        }

        /// <summary>Deliver the given inbound publish event args to whatever feature clients have subscribed to this connection.</summary>
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
