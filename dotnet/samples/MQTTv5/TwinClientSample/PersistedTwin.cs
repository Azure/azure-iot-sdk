// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

/// <summary>
/// This device's copy of its twin, persisted to disk so that it survives restarts of the application.
/// </summary>
/// <remarks>
/// <para>
/// The SDK does not persist the twin. An application that does can tell the SDK which versions of the desired and reported
/// properties it holds when it starts, so that IoT hub only pushes the sections that changed while the device was offline.
/// </para>
/// <para>
/// Versions are never 0 in IoT hub, so a version of 0 means this device doesn't hold that section in full.
/// </para>
/// <para>
/// Reported properties are kept as IoT hub's last known version of them plus the patches that this device has made since,
/// which IoT hub hasn't applied yet. The pending patches are persisted too, so that they are sent again after a restart.
/// </para>
/// </remarks>
internal sealed class PersistedTwin
{
    private static readonly JsonSerializerOptions s_serializerOptions = new() { WriteIndented = true };

    private readonly object _lock = new();
    private readonly string _path;
    private State _state;

    private PersistedTwin(string path, State state)
    {
        _path = path;
        _state = state;
    }

    public ulong DesiredVersion
    {
        get { lock (_lock) { return _state.DesiredVersion; } }
    }

    public ulong ReportedVersion
    {
        get { lock (_lock) { return _state.ReportedVersion; } }
    }

    /// <summary>
    /// Loads the twin persisted at the given path, or starts with an empty twin that this device holds no versions of.
    /// </summary>
    public static PersistedTwin Load(string path)
    {
        State? state = null;
        if (File.Exists(path))
        {
            try
            {
                state = JsonSerializer.Deserialize<State>(File.ReadAllText(path));
            }
            catch (JsonException e)
            {
                // Holding no versions is always safe: IoT hub pushes both sections upon connecting.
                Console.WriteLine($"Ignoring the unreadable persisted twin at {path}: {e.Message}");
            }
        }

        return new PersistedTwin(path, state ?? new State());
    }

    /// <summary>
    /// Applies a desired properties patch. The SDK only delivers each patch once, in version order.
    /// </summary>
    public void ApplyDesiredPatch(ulong version, JsonObject patch)
    {
        lock (_lock)
        {
            ApplyMergePatch(_state.Desired, patch);
            _state.DesiredVersion = version;
            Save();
        }
    }

    /// <summary>
    /// Replaces the desired or reported properties with a full snapshot of them from IoT hub.
    /// </summary>
    public void ApplySnapshot(bool desired, ulong version, JsonObject properties)
    {
        lock (_lock)
        {
            if (desired)
            {
                _state.Desired = (JsonObject)properties.DeepClone();
                _state.DesiredVersion = version;
            }
            else
            {
                _state.Reported = (JsonObject)properties.DeepClone();
                _state.ReportedVersion = version;
            }

            Save();
        }
    }

    /// <summary>
    /// Records a reported properties patch that this device is about to send, so that it is sent again if the
    /// application restarts before IoT hub applies it. Pending patches must be sent in the order they were recorded.
    /// </summary>
    public PendingPatch AddPendingReportedPatch(JsonObject patch)
    {
        lock (_lock)
        {
            PendingPatch pending = new() { Patch = (JsonObject)patch.DeepClone() };
            _state.PendingReported.Add(pending);
            Save();
            return pending;
        }
    }

    public IReadOnlyList<PendingPatch> GetPendingReportedPatches()
    {
        lock (_lock)
        {
            return _state.PendingReported.ToList();
        }
    }

    /// <summary>
    /// Records that IoT hub applied a pending reported properties patch at the given version.
    /// </summary>
    public void CompleteReportedPatch(PendingPatch applied, ulong version)
    {
        lock (_lock)
        {
            int index = _state.PendingReported.IndexOf(applied);
            if (index < 0)
            {
                // Already folded in when a later patch that was sent along with it completed.
                return;
            }

            // The SDK sends pending patches in order, combining those made while another is in flight, so IoT hub has applied
            // every patch made before this one too.
            foreach (PendingPatch pending in _state.PendingReported.Take(index + 1))
            {
                ApplyMergePatch(_state.Reported, pending.Patch);
            }

            _state.PendingReported.RemoveRange(0, index + 1);

            // This device only holds IoT hub's version of the reported properties in full if it held the version before.
            if (_state.ReportedVersion != 0)
            {
                _state.ReportedVersion = Math.Max(_state.ReportedVersion, version);
            }

            Save();
        }
    }

    /// <summary>
    /// Records that IoT hub rejected a pending reported properties patch, so it won't be sent again.
    /// </summary>
    public void RejectReportedPatch(PendingPatch rejected)
    {
        lock (_lock)
        {
            _state.PendingReported.Remove(rejected);

            // A later patch may have already folded this one into this device's copy of the reported properties, so this
            // device no longer knows that its copy matches IoT hub's.
            _state.ReportedVersion = 0;
            Save();
        }
    }

    public string ToJsonString()
    {
        lock (_lock)
        {
            return JsonSerializer.Serialize(_state, s_serializerOptions);
        }
    }

    // Must be called while holding _lock. Writes to a temporary file first so that a crash mid-write never leaves a corrupt twin.
    private void Save()
    {
        string temporaryPath = _path + ".tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(_state, s_serializerOptions));
        File.Move(temporaryPath, _path, overwrite: true);
    }

    // Twin patches are JSON merge patches (RFC 7396): null removes a property, and objects are merged recursively.
    private static void ApplyMergePatch(JsonObject target, JsonObject patch)
    {
        foreach (KeyValuePair<string, JsonNode?> property in patch)
        {
            if (property.Value == null)
            {
                target.Remove(property.Key);
            }
            else if (property.Value is JsonObject patchChild)
            {
                if (target[property.Key] is not JsonObject targetChild)
                {
                    targetChild = new JsonObject();
                    target[property.Key] = targetChild;
                }

                ApplyMergePatch(targetChild, patchChild);
            }
            else
            {
                target[property.Key] = property.Value.DeepClone();
            }
        }
    }

    internal sealed class PendingPatch
    {
        public required JsonObject Patch { get; init; }
    }

    private sealed class State
    {
        public ulong DesiredVersion { get; set; }

        public JsonObject Desired { get; set; } = new();

        public ulong ReportedVersion { get; set; }

        public JsonObject Reported { get; set; } = new();

        public List<PendingPatch> PendingReported { get; set; } = new();
    }
}
