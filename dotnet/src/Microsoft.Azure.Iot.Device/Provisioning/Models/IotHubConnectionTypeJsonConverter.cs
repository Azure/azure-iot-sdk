// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Azure.Iot.Device.Models;

namespace Microsoft.Azure.Iot.Device.Provisioning.Models
{
    /// <summary>
    /// Serializes the public <see cref="IotHubConnectionType"/> through the hidden <see cref="ConnectionProfile"/>
    /// wire enum so the on-the-wire values ("classic", "mqttV5") are preserved even though the public API exposes the
    /// "Mqttv3"/"Mqttv5" surface.
    /// </summary>
    internal sealed class IotHubConnectionTypeJsonConverter : JsonConverter<IotHubConnectionType>
    {
        public override IotHubConnectionType Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
            => JsonSerializer.Deserialize<ConnectionProfile>(ref reader, options).ToIotHubConnectionType();

        public override void Write(Utf8JsonWriter writer, IotHubConnectionType value, JsonSerializerOptions options)
            => JsonSerializer.Serialize(writer, value.ToConnectionProfile(), options);
    }
}
