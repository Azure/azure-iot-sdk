// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Events;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Unified.Connection
{
    // A stub class that is for converting Unified connection client traffic into MQTTv5 client traffic
    internal class Stub : MQTTv5.Connection.IConnectionClient
    {
        private readonly AsyncEvent<MqttPublishReceivedEventArgs> _publishReceived = new();

        public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync
        {
            add => _publishReceived.Add(value);
            remove => _publishReceived.Remove(value);
        }

        private readonly Unified.Connection.IConnectionClient _unifiedConnectionClient;

        internal Stub(Unified.Connection.IConnectionClient unifiedConnectionClient)
        {
            _unifiedConnectionClient = unifiedConnectionClient;
            _unifiedConnectionClient.PublishReceivedAsync += ForwardPublishReceivedAsync;
        }

        // Forward received publishes through a stable method reference. Subscribing the 'PublishReceivedAsync' event
        // field directly would capture its (null) backing delegate at construction time, so later subscribers (the
        // underlying MQTTv5 feature clients) would never be invoked.
        private Task ForwardPublishReceivedAsync(MqttPublishReceivedEventArgs args)
        {
            return _publishReceived.InvokeAsync(args);
        }

        // Stops forwarding received publishes, without disposing the unified connection client.
        internal void Detach()
        {
            _unifiedConnectionClient.PublishReceivedAsync -= ForwardPublishReceivedAsync;
        }

        public void Dispose()
        {
            Detach();
            _unifiedConnectionClient.Dispose();
        }

        public ConnectionContext? GetCurrentConnectionContext()
        {
            return _unifiedConnectionClient.GetCurrentConnectionContext();
        }

        public bool IsConnectionTargetSet => _unifiedConnectionClient.IsConnectionTargetSet;

        Guid? MQTTv5.Connection.IConnectionClient.ConnectionNonce => _unifiedConnectionClient.ConnectionNonce;

        event Action? MQTTv5.Connection.IConnectionClient.ConnectionLost
        {
            add => _unifiedConnectionClient.ConnectionLost += value;
            remove => _unifiedConnectionClient.ConnectionLost -= value;
        }

        MQTTv5.Connection.BirthAckVersions? MQTTv5.Connection.IConnectionClient.LatestBirthAck => _unifiedConnectionClient.LatestBirthAck;

        bool MQTTv5.Connection.IConnectionClient.IsBirthAckTimedOut(Guid connectionNonce) => _unifiedConnectionClient.IsBirthAckTimedOut(connectionNonce);

        event Action? MQTTv5.Connection.IConnectionClient.TwinIdentityChanged
        {
            add => _unifiedConnectionClient.TwinIdentityChanged += value;
            remove => _unifiedConnectionClient.TwinIdentityChanged -= value;
        }

        ulong MQTTv5.Connection.IConnectionClient.InitialDesiredPropertiesVersion => _unifiedConnectionClient.InitialDesiredPropertiesVersion;

        ulong MQTTv5.Connection.IConnectionClient.InitialReportedPropertiesVersion => _unifiedConnectionClient.InitialReportedPropertiesVersion;

        void MQTTv5.Connection.IConnectionClient.ObserveDesiredPropertiesVersion(ulong version) => _unifiedConnectionClient.ObserveDesiredPropertiesVersion(version);

        void MQTTv5.Connection.IConnectionClient.ObserveReportedPropertiesVersion(ulong version) => _unifiedConnectionClient.ObserveReportedPropertiesVersion(version);

        void MQTTv5.Connection.IConnectionClient.ReplaceDesiredPropertiesVersion(ulong version) => _unifiedConnectionClient.ReplaceDesiredPropertiesVersion(version);

        void MQTTv5.Connection.IConnectionClient.ReplaceReportedPropertiesVersion(ulong version) => _unifiedConnectionClient.ReplaceReportedPropertiesVersion(version);

        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, bool throwOnErrorReasonCode, CancellationToken cancellationToken = default)
        {
            return _unifiedConnectionClient.PublishAsync(publish, throwOnErrorReasonCode, cancellationToken);
        }

        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, bool throwOnErrorReasonCode, CancellationToken cancellationToken = default)
        {
            return _unifiedConnectionClient.SubscribeAsync(subscribe, throwOnErrorReasonCode, cancellationToken);
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, bool throwOnErrorReasonCode, CancellationToken cancellationToken = default)
        {
            return _unifiedConnectionClient.UnsubscribeAsync(unsubscribe, throwOnErrorReasonCode, cancellationToken);
        }
    }
}
