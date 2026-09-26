// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System.Text.Json;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class DeviceRegistrationResultUnitTests
    {
        [Fact]
        public void DeserializeConnectionProfileWhenSpecifiedUsesProvidedValue()
        {
            const string json = """{"connectionProfile":"mqttV5"}""";

            DeviceRegistrationResult? result =
                JsonSerializer.Deserialize<DeviceRegistrationResult>(json, JsonSerializationSettings.Options);

            Assert.NotNull(result);
            Assert.Equal(ConnectionProfile.MqttV5, result.ConnectionProfile);
        }

        [Fact]
        public void DeserializeConnectionProfileWhenUnspecifiedDefaultsToClassic()
        {
            const string json = "{}";

            DeviceRegistrationResult? result =
                JsonSerializer.Deserialize<DeviceRegistrationResult>(json, JsonSerializationSettings.Options);

            Assert.NotNull(result);
            Assert.Equal(ConnectionProfile.Classic, result.ConnectionProfile);
        }
    }
}
