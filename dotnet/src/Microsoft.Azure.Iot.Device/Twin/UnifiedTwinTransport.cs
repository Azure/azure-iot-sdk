// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using IUnifiedConnectionClient = Microsoft.Azure.Iot.Device.Unified.Connection.IConnectionClient;

namespace Microsoft.Azure.Iot.Device.Twin
{
    // The twin transport of a unified connection, which may connect to IoT hubs that use MQTTv3 or MQTTv5, and may move from
    // one kind to the other when the device is re-provisioned. Each connection uses the transport of the IoT hub's MQTT
    // version, chosen from the current connection's profile. Requests in flight when the connection is lost are abandoned
    // (and retried by the engine) through the connection lost event, as they are for any other reconnection.
    internal sealed class UnifiedTwinTransport : ITwinTransport
    {
        private readonly IUnifiedConnectionClient _connection;

        private readonly ClassicTwinTransport _classicTransport;

        // Adapts the unified connection to the MQTTv5 transport.
        private readonly Stub _mqttv5Connection;

        private readonly Mqttv5TwinTransport _mqttv5Transport;

        private ITwinTransportHandler? _handler;

        public UnifiedTwinTransport(IUnifiedConnectionClient connection)
        {
            _connection = connection;
            _classicTransport = new ClassicTwinTransport(connection);
            _mqttv5Connection = new Stub(connection);
            _mqttv5Transport = new Mqttv5TwinTransport(_mqttv5Connection, handlesConnectionEvents: false);
        }

        private bool IsConnectedToMqttv5Hub => _connection.GetCurrentConnectionContext()?.ConnectionProfile == IotHubConnectionType.Mqttv5;

        private ITwinTransport Active => IsConnectedToMqttv5Hub ? _mqttv5Transport : _classicTransport;

        public bool AnnouncesTwinVersions => Active.AnnouncesTwinVersions;

        public Guid? ConnectionNonce => Active.ConnectionNonce;

        public bool IsConnected => Active.IsConnected;

        public bool IsReady => Active.IsReady;

        public BirthAckVersions? LatestBirthAck => Active.LatestBirthAck;

        public bool IsBirthAckTimedOut(Guid connectionNonce) => Active.IsBirthAckTimedOut(connectionNonce);

        public ulong InitialDesiredPropertiesVersion => _connection.InitialDesiredPropertiesVersion;

        public ulong InitialReportedPropertiesVersion => _connection.InitialReportedPropertiesVersion;

        public void ObserveDesiredPropertiesVersion(ulong version) => _connection.ObserveDesiredPropertiesVersion(version);

        public void ObserveReportedPropertiesVersion(ulong version) => _connection.ObserveReportedPropertiesVersion(version);

        public void ReplaceDesiredPropertiesVersion(ulong version) => _connection.ReplaceDesiredPropertiesVersion(version);

        public void ReplaceReportedPropertiesVersion(ulong version) => _connection.ReplaceReportedPropertiesVersion(version);

        public Task PublishGetAsync(Guid getId, bool getDesired, bool getReported, ulong ifNotMatchDesired, ulong ifNotMatchReported, CancellationToken cancellationToken)
        {
            return Active.PublishGetAsync(getId, getDesired, getReported, ifNotMatchDesired, ifNotMatchReported, cancellationToken);
        }

        public Task PublishReportedPatchAsync(Guid patchId, ulong ifMatch, byte[] payload, CancellationToken cancellationToken)
        {
            return Active.PublishReportedPatchAsync(patchId, ifMatch, payload, cancellationToken);
        }

        public void Start(ITwinTransportHandler handler)
        {
            _handler = handler;
            _classicTransport.Start(handler);
            _mqttv5Transport.Start(handler);
            _connection.ConnectionLost += HandleConnectionLost;
            _connection.ConnectionReady += handler.HandleConnectionReady;
            _connection.TwinIdentityChanged += handler.HandleTwinIdentityChanged;
        }

        public void Detach()
        {
            _classicTransport.Detach();
            _mqttv5Transport.Detach();

            // Only stops forwarding received publishes. Disposing the stub would dispose the connection.
            _mqttv5Connection.Detach();
            if (_handler != null)
            {
                _connection.ConnectionLost -= HandleConnectionLost;
                _connection.ConnectionReady -= _handler.HandleConnectionReady;
                _connection.TwinIdentityChanged -= _handler.HandleTwinIdentityChanged;
            }
        }

        private void HandleConnectionLost()
        {
            // The classic transport's nonce is renewed for every lost connection, whichever kind of IoT hub it was to, so
            // that nothing received over an earlier classic connection is mistaken for part of the next one.
            Guid lostClassicConnectionNonce = _classicTransport.RenewConnectionNonce();
            Guid? lostConnectionNonce = IsConnectedToMqttv5Hub ? _connection.ConnectionNonce : lostClassicConnectionNonce;
            _handler?.HandleConnectionLost(lostConnectionNonce);
        }
    }
}
