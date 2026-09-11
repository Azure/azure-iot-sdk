using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The stub hub's authoritative server-side state for a single device.
    /// </summary>
    /// <remarks>
    /// Per the twin design doc section 1, twin state is split into two independently versioned sections, and the authoritative
    /// initial version for both sections is 1 with empty contents. Version 0 is reserved as the device-side "I have never seen
    /// authoritative twin state" sentinel and is therefore never used by this class.
    /// </remarks>
    public sealed class StubDeviceState
    {
        private readonly Lock _stateLock = new();

        internal StubDeviceState(string deviceId)
        {
            DeviceId = deviceId;
        }

        /// <summary>
        /// The device id this state belongs to.
        /// </summary>
        public string DeviceId { get; }

        /// <summary>
        /// The authoritative desired properties.
        /// </summary>
        public JsonObject Desired { get; private set; } = new();

        /// <summary>
        /// The authoritative desired properties version.
        /// </summary>
        public ulong DesiredVersion { get; private set; } = 1;

        /// <summary>
        /// The authoritative reported properties.
        /// </summary>
        public JsonObject Reported { get; private set; } = new();

        /// <summary>
        /// The authoritative reported properties version.
        /// </summary>
        public ulong ReportedVersion { get; private set; } = 1;

        /// <summary>
        /// The connection nonce from the most recently admitted birth, or null if this device has not birthed yet.
        /// Gen2 only. Backend-initiated twin dispatch carries this value as MQTT 5 correlation data.
        /// </summary>
        public Guid? ConnectionNonce { get; internal set; }

        /// <summary>
        /// Whether the most recently admitted birth requested backend push of desired properties. Gen2 only.
        /// </summary>
        public bool PushDesired { get; internal set; }

        /// <summary>
        /// Whether the most recently admitted birth requested backend push of reported properties. Gen2 only.
        /// </summary>
        public bool PushReported { get; internal set; }

        /// <summary>
        /// Whether this device has completed the presence handshake and is therefore eligible for backend-initiated dispatch.
        /// Gen2 only; always true for gen1, which has no presence handshake.
        /// </summary>
        public bool IsDispatchReady { get; internal set; }

        /// <summary>
        /// Take a consistent snapshot of both twin sections.
        /// </summary>
        public StubTwinSnapshot GetTwinSnapshot()
        {
            lock (_stateLock)
            {
                return new StubTwinSnapshot(
                    (JsonObject)Desired.DeepClone(),
                    DesiredVersion,
                    (JsonObject)Reported.DeepClone(),
                    ReportedVersion);
            }
        }

        /// <summary>
        /// Replace the desired properties outright and bump the desired version. Mirrors a service caller replacing the
        /// desired section.
        /// </summary>
        /// <returns>The new desired properties version.</returns>
        public ulong ReplaceDesiredProperties(JsonObject desiredProperties)
        {
            ArgumentNullException.ThrowIfNull(desiredProperties);

            lock (_stateLock)
            {
                Desired = (JsonObject)desiredProperties.DeepClone();
                DesiredVersion++;
                return DesiredVersion;
            }
        }

        /// <summary>
        /// Apply a JSON merge patch to the desired properties and bump the desired version.
        /// </summary>
        /// <returns>The new desired properties version.</returns>
        public ulong ApplyDesiredPatch(JsonObject patch)
        {
            ArgumentNullException.ThrowIfNull(patch);

            lock (_stateLock)
            {
                Desired = JsonMergePatch.Apply(Desired, patch);
                DesiredVersion++;
                return DesiredVersion;
            }
        }

        /// <summary>
        /// Apply a device-sent reported-properties patch under optimistic concurrency.
        /// </summary>
        /// <param name="patch">The patch to merge into the reported section.</param>
        /// <param name="ifMatch">
        /// The device's view of the authoritative reported version. A value of 0 means "unconditional", which is what gen1
        /// devices always send since classic IoT hub reported writes are unconditional.
        /// </param>
        /// <param name="newVersion">The resulting reported version. On a version mismatch this is the unchanged current version.</param>
        /// <returns>True if the patch was applied, false if <paramref name="ifMatch"/> did not match the authoritative version.</returns>
        internal bool TryApplyReportedPatch(JsonObject patch, ulong ifMatch, out ulong newVersion)
        {
            ArgumentNullException.ThrowIfNull(patch);

            lock (_stateLock)
            {
                if (ifMatch != 0 && ifMatch != ReportedVersion)
                {
                    newVersion = ReportedVersion;
                    return false;
                }

                Reported = JsonMergePatch.Apply(Reported, patch);
                ReportedVersion++;
                newVersion = ReportedVersion;
                return true;
            }
        }
    }

    /// <summary>
    /// An immutable snapshot of a device's twin as held by the stub hub.
    /// </summary>
    /// <param name="Desired">The desired properties, without any <c>$version</c> metadata key.</param>
    /// <param name="DesiredVersion">The authoritative desired properties version.</param>
    /// <param name="Reported">The reported properties, without any <c>$version</c> metadata key.</param>
    /// <param name="ReportedVersion">The authoritative reported properties version.</param>
    public sealed record StubTwinSnapshot(JsonObject Desired, ulong DesiredVersion, JsonObject Reported, ulong ReportedVersion);

    /// <summary>
    /// The JSON merge semantics that IoT hub twins use: a null value removes the key, a nested object merges recursively, and
    /// any other value replaces.
    /// </summary>
    internal static class JsonMergePatch
    {
        internal static JsonObject Apply(JsonObject target, JsonObject patch)
        {
            var result = (JsonObject)target.DeepClone();

            foreach (KeyValuePair<string, JsonNode?> property in patch)
            {
                if (property.Value == null)
                {
                    result.Remove(property.Key);
                }
                else if (property.Value is JsonObject nestedPatch
                    && result.TryGetPropertyValue(property.Key, out JsonNode? existing)
                    && existing is JsonObject nestedTarget)
                {
                    result[property.Key] = Apply(nestedTarget, nestedPatch);
                }
                else
                {
                    result[property.Key] = property.Value.DeepClone();
                }
            }

            return result;
        }
    }
}
