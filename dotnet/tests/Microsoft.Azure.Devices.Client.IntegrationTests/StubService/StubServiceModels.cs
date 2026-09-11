using System.Text.Json.Nodes;
using MethodsProto = Microsoft.Azure.Devices.Client.Models.DirectMethods;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// Raised when the stub hub receives device-to-cloud telemetry.
    /// </summary>
    public sealed class StubTelemetryReceivedEventArgs : EventArgs
    {
        /// <summary>
        /// The device that sent the telemetry.
        /// </summary>
        public required string DeviceId { get; init; }

        /// <summary>
        /// The raw telemetry payload.
        /// </summary>
        public required byte[] Payload { get; init; }

        /// <summary>
        /// The message id, if the device supplied one.
        /// </summary>
        public string? MessageId { get; init; }

        /// <summary>
        /// The correlation id, if the device supplied one.
        /// </summary>
        public string? CorrelationId { get; init; }

        /// <summary>
        /// The content type, if the device supplied one.
        /// </summary>
        public string? ContentType { get; init; }

        /// <summary>
        /// The content encoding, if the device supplied one.
        /// </summary>
        public string? ContentEncoding { get; init; }

        /// <summary>
        /// The custom application properties the device supplied.
        /// </summary>
        public IReadOnlyDictionary<string, string> UserProperties { get; init; } = new Dictionary<string, string>();
    }

    /// <summary>
    /// Raised when the stub hub receives, and finishes applying, a device's reported-properties patch.
    /// </summary>
    public sealed class StubReportedPropertiesReceivedEventArgs : EventArgs
    {
        /// <summary>
        /// The device that sent the patch.
        /// </summary>
        public required string DeviceId { get; init; }

        /// <summary>
        /// The patch the device sent.
        /// </summary>
        public required JsonObject Patch { get; init; }

        /// <summary>
        /// The <c>if_match</c> version the device supplied. Always 0 for gen1, which has no optimistic concurrency.
        /// </summary>
        public ulong IfMatch { get; init; }

        /// <summary>
        /// Whether the stub applied the patch. False means the device's <see cref="IfMatch"/> lost the optimistic
        /// concurrency check.
        /// </summary>
        public required bool WasApplied { get; init; }

        /// <summary>
        /// The authoritative reported version after handling this patch.
        /// </summary>
        public required ulong ReportedVersion { get; init; }
    }

    /// <summary>
    /// Raised when the stub hub receives a device's twin GET request.
    /// </summary>
    public sealed class StubTwinGetReceivedEventArgs : EventArgs
    {
        /// <summary>
        /// The device that requested the twin.
        /// </summary>
        public required string DeviceId { get; init; }

        /// <summary>
        /// Whether the device asked for the desired section.
        /// </summary>
        public required bool RequestedDesired { get; init; }

        /// <summary>
        /// Whether the device asked for the reported section.
        /// </summary>
        public required bool RequestedReported { get; init; }
    }

    /// <summary>
    /// Raised when the stub hub admits a device's birth message. Gen2 only.
    /// </summary>
    public sealed class StubDeviceBirthEventArgs : EventArgs
    {
        /// <summary>
        /// The device that birthed.
        /// </summary>
        public required string DeviceId { get; init; }

        /// <summary>
        /// The connection nonce the device generated for this connection attempt.
        /// </summary>
        public required Guid ConnectionNonce { get; init; }

        /// <summary>
        /// The session-present flag the device observed in its CONNACK. Diagnostic only; per the presence design doc section 5.2
        /// the backend must not make state machine decisions from it.
        /// </summary>
        public required bool SessionPresent { get; init; }

        /// <summary>
        /// The desired-properties version the device claims to hold.
        /// </summary>
        public required ulong DeviceDesiredVersion { get; init; }

        /// <summary>
        /// The reported-properties version the device claims to hold.
        /// </summary>
        public required ulong DeviceReportedVersion { get; init; }

        /// <summary>
        /// Whether the device requested backend push of desired properties.
        /// </summary>
        public required bool PushDesired { get; init; }

        /// <summary>
        /// Whether the device requested backend push of reported properties.
        /// </summary>
        public required bool PushReported { get; init; }
    }

    /// <summary>
    /// How a direct method invocation issued by the stub hub ended.
    /// </summary>
    public enum StubDirectMethodOutcome
    {
        /// <summary>
        /// The device executed the method and returned a result.
        /// </summary>
        Completed,

        /// <summary>
        /// The device declined the delivery probe. Gen2 only.
        /// </summary>
        Rejected,

        /// <summary>
        /// The device terminated its ready token before execution began. Gen2 only.
        /// </summary>
        Abandoned,

        /// <summary>
        /// The device did not respond within the configured budget.
        /// </summary>
        TimedOut,
    }

    /// <summary>
    /// The outcome of a direct method the stub hub invoked on a device.
    /// </summary>
    public sealed class StubDirectMethodResult
    {
        /// <summary>
        /// How the invocation ended.
        /// </summary>
        public required StubDirectMethodOutcome Outcome { get; init; }

        /// <summary>
        /// The application-defined status the device returned. Only meaningful when <see cref="Outcome"/> is
        /// <see cref="StubDirectMethodOutcome.Completed"/>.
        /// </summary>
        public int Status { get; init; }

        /// <summary>
        /// The result body the device returned. Only meaningful when <see cref="Outcome"/> is
        /// <see cref="StubDirectMethodOutcome.Completed"/>.
        /// </summary>
        public byte[] Payload { get; init; } = Array.Empty<byte>();

        /// <summary>
        /// Why the device declined the delivery probe. Only set when <see cref="Outcome"/> is
        /// <see cref="StubDirectMethodOutcome.Rejected"/>.
        /// </summary>
        public MethodsProto.RejectedReason? RejectedReason { get; init; }

        /// <summary>
        /// Why the device abandoned its ready token. Only set when <see cref="Outcome"/> is
        /// <see cref="StubDirectMethodOutcome.Abandoned"/>.
        /// </summary>
        public MethodsProto.AbandonReason? AbandonReason { get; init; }
    }

    /// <summary>
    /// A cloud-to-device message the stub hub sends to a device. Gen1 only; the gen2 cloud-to-device protocol is not defined yet.
    /// </summary>
    public sealed class StubCloudToDeviceMessage
    {
        /// <summary>
        /// The message body.
        /// </summary>
        public byte[] Payload { get; set; } = Array.Empty<byte>();

        /// <summary>
        /// The message id. If left null, the stub generates one, because the classic device-bound topic always carries a
        /// property bag and the device SDK requires that topic segment to be present.
        /// </summary>
        public string? MessageId { get; set; }

        /// <summary>
        /// The correlation id.
        /// </summary>
        public string? CorrelationId { get; set; }

        /// <summary>
        /// The content type.
        /// </summary>
        public string? ContentType { get; set; }

        /// <summary>
        /// The content encoding.
        /// </summary>
        public string? ContentEncoding { get; set; }

        /// <summary>
        /// Custom application properties.
        /// </summary>
        public IDictionary<string, string> UserProperties { get; } = new Dictionary<string, string>();
    }
}
