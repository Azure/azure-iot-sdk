// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Mqtt;
using System.Diagnostics.CodeAnalysis;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// Recognizes the service bound publishes that the SDK sends to an MQTTv5 IoT hub, in either
    /// <see cref="MQTTv5.Models.ServiceBoundTopicFormat"/>.
    /// </summary>
    internal static class ServiceBoundTestTopics
    {
        private const string PerDevicePrefix = "ih/srv/";
        private const string LegacyBirthSuffix = "/srv/presence";

        /// <summary>
        /// Whether this publish is a birth: either on the per-device topic <c>ih/srv/{deviceId}</c> with user property
        /// <c>type=birth:1</c>, or on the legacy birth topic <c>ih/{deviceId}/srv/presence</c>.
        /// </summary>
        public static bool IsBirth([NotNullWhen(true)] MqttPublish? publish)
        {
            return publish != null && TryGetBirthDeviceId(publish, out _);
        }

        /// <summary>Whether this publish is a birth from the given device. See <see cref="IsBirth"/>.</summary>
        public static bool IsBirth([NotNullWhen(true)] MqttPublish? publish, string deviceId)
        {
            return publish != null && TryGetBirthDeviceId(publish, out string? birthDeviceId) && birthDeviceId == deviceId;
        }

        /// <summary>
        /// If this publish is a birth (see <see cref="IsBirth"/>), the ID of the device it came from, which is the topic's
        /// third segment for the per-device topic, or its second segment for the legacy one.
        /// </summary>
        public static bool TryGetBirthDeviceId(MqttPublish publish, [NotNullWhen(true)] out string? deviceId)
        {
            deviceId = null;
            string topic = publish.Topic;
            if (topic.StartsWith(PerDevicePrefix, StringComparison.Ordinal))
            {
                string[] segments = topic.Split('/');
                if (segments.Length == 3
                    && segments[2].Length > 0
                    && publish.UserProperties.TryGetProperty("type", out string? type)
                    && type == "birth:1")
                {
                    deviceId = segments[2];
                    return true;
                }

                return false;
            }

            if (topic.StartsWith("ih/", StringComparison.Ordinal) && topic.EndsWith(LegacyBirthSuffix, StringComparison.Ordinal))
            {
                deviceId = topic.Split('/')[1];
                return true;
            }

            return false;
        }
    }
}
