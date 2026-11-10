// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.MQTTv5.Models
{
    /// <summary>
    /// The topics that a device publishes its births, direct method responses and twin requests on, when connected to an
    /// IoT hub over MQTTv5.
    /// </summary>
    /// <remarks>
    /// Telemetry is always published on "ih/{deviceId}/srv/telemetry", and the device always subscribes to
    /// "ih/{deviceId}/dev/#", whichever format is chosen.
    /// </remarks>
    public enum ServiceBoundTopicFormat
    {
        /// <summary>
        /// Publish births, direct method responses and twin requests on the device's own topic, "ih/srv/{deviceId}". The
        /// "type" user property tells them apart. This is the default, and requires an IoT hub that supports it.
        /// </summary>
        PerDevice = 0,

        /// <summary>
        /// Publish births on "ih/{deviceId}/srv/presence", direct method responses on "ih/{deviceId}/srv/methods" and twin
        /// requests on "ih/{deviceId}/srv/twin", for IoT hubs that don't support <see cref="PerDevice"/>.
        /// </summary>
        Legacy = 1,
    }

    internal static class ServiceBoundTopics
    {
        internal const string Presence = "presence";
        internal const string Methods = "methods";
        internal const string Twin = "twin";

        /// <summary>
        /// The topic to publish a service bound message of the given kind on.
        /// </summary>
        /// <param name="format">The topic format the connection client was configured with.</param>
        /// <param name="deviceId">The device's ID, or the device ID placeholder that the connection client resolves.</param>
        /// <param name="legacyTopicSuffix">The last segment of the legacy topic, such as <see cref="Presence"/>.</param>
        internal static string Get(ServiceBoundTopicFormat format, string deviceId, string legacyTopicSuffix)
        {
            return format == ServiceBoundTopicFormat.Legacy
                ? "ih/" + deviceId + "/srv/" + legacyTopicSuffix
                : "ih/srv/" + deviceId;
        }
    }
}
