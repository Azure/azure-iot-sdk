// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Models
{
    public class SimpleDirectMethodPayload
    {
        [JsonPropertyName("someInt")]
        public int? SomeInt { get; set; }

        [JsonPropertyName("someString")]
        public string? SomeString { get; set; }

        public static SimpleDirectMethodPayload FromJsonBytes(byte[] json)
        {
            return JsonSerializer.Deserialize<SimpleDirectMethodPayload>(json) ?? throw new ArgumentException("Could not deserialize the payload");
        }

        public static SimpleDirectMethodPayload FromJson(string json)
        {
            return JsonSerializer.Deserialize<SimpleDirectMethodPayload>(json) ?? throw new ArgumentException("Could not deserialize the payload");
        }

        public byte[] ToJsonByteArray()
        {
            return JsonSerializer.SerializeToUtf8Bytes<SimpleDirectMethodPayload>(this);
        }

        public string ToJson()
        {
            return JsonSerializer.Serialize<SimpleDirectMethodPayload>(this);
        }
    }
}
