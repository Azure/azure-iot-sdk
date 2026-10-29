// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Twin
{
    // Sends the twin requests of a TwinEngine in one IoT hub generation's wire format, and passes the twin messages that it
    // receives to the engine (see ITwinTransportHandler) as they arrive, one at a time and in the order received.
    internal interface ITwinTransport
    {
        // Whether the current connection announces this device's twin versions in its birth message, and IoT hub reveals its
        // own versions in the birth ack and pushes the twin sections that changed. If false, IoT hub never pushes the twin
        // or acknowledges the device's presence, so the engine gets the full twin each time the connection is ready instead.
        // Reported properties patches are only sent after the current connection's birth ack if this is true.
        bool AnnouncesTwinVersions { get; }

        // Identifies the current (or most recent) connection, so that the engine tells the state of one connection from that
        // of the next. Null if it can't tell them apart.
        Guid? ConnectionNonce { get; }

        // Whether the device is connected (or reconnecting), so that the application's get twin requests can be sent.
        bool IsConnected { get; }

        // Whether reported properties patches can be sent now (subject to the birth ack, if AnnouncesTwinVersions).
        bool IsReady { get; }

        BirthAckVersions? LatestBirthAck { get; }

        bool IsBirthAckTimedOut(Guid connectionNonce);

        ulong InitialDesiredPropertiesVersion { get; }

        ulong InitialReportedPropertiesVersion { get; }

        // The versions that the device holds, which a transport that announces them includes in the next birth message.
        void ObserveDesiredPropertiesVersion(ulong version);

        void ObserveReportedPropertiesVersion(ulong version);

        void ReplaceDesiredPropertiesVersion(ulong version);

        void ReplaceReportedPropertiesVersion(ulong version);

        // Transports that can't request just one section (or make the request conditional) get the full twin. The engine
        // only uses the sections that were requested.
        Task PublishGetAsync(Guid getId, bool getDesired, bool getReported, ulong ifNotMatchDesired, ulong ifNotMatchReported, CancellationToken cancellationToken);

        // An if-match of 0 makes the patch unconditional. Transports that can't make a patch conditional ignore it.
        Task PublishReportedPatchAsync(Guid patchId, ulong ifMatch, byte[] payload, CancellationToken cancellationToken);

        // Starts passing received twin messages (and connection events, if this transport handles them) to the handler.
        void Start(ITwinTransportHandler handler);

        // Stops passing anything to the handler. Doesn't dispose the connection.
        void Detach();
    }

    // Implemented by TwinEngine. Each received message is awaited before the next one is passed.
    internal interface ITwinTransportHandler
    {
        // A response to a get twin request or a reported properties patch, correlated by the ID it was sent with.
        Task HandleResponseAsync(Guid? connectionNonce, Guid requestId, TwinResponse response);

        Task HandleDesiredPatchAsync(Guid? connectionNonce, ReceivedDesiredPatch patch);

        Task HandleTwinPushAsync(Guid? connectionNonce, TwinPushSection? desired, TwinPushSection? reported);

        Task HandleBirthAckAsync(Guid? connectionNonce, ulong desiredVersion, ulong reportedVersion);

        // The nonce identifies the connection that was lost.
        void HandleConnectionLost(Guid? lostConnectionNonce);

        void HandleConnectionReady();

        void HandleTwinIdentityChanged();
    }

    internal enum TwinResponseStatus
    {
        Success,

        // IoT hub is throttling or failing, so the request may succeed if it is sent again later.
        Transient,

        Rejected,
    }

    // A response whose meaning depends on the request it correlates to. A transport that can't tell which kind of request
    // a response is for (such as one for IoT hubs that use MQTTv3) sets both ParseSnapshot and PatchResponse.
    internal sealed class TwinResponse
    {
        public TwinResponseStatus Status { get; init; }

        public int StatusCode { get; init; }

        // Parses the twin sections in a get twin response. Throws if the response is malformed.
        public Func<TwinSnapshot>? ParseSnapshot { get; init; }

        // The outcome of a reported properties patch.
        public ReportedPatchResponse? PatchResponse { get; init; }
    }

    // The twin sections in a get twin response. A section is null if the response doesn't include it.
    internal sealed record TwinSnapshot(JsonObject? Desired, ulong DesiredVersion, JsonObject? Reported, ulong ReportedVersion);

    // A desired properties patch, or a version probe if Properties is null.
    internal sealed record ReceivedDesiredPatch(ulong Version, JsonObject? Properties);
}
