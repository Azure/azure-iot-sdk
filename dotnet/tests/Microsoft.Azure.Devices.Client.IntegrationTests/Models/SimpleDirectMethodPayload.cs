using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Models
{
    public class SimpleDirectMethodPayload
    {
        [JsonPropertyName("someInt")]
        public int SomeInt { get; set; }

        [JsonPropertyName("someString")]
        public string SomeString { get; set; }

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
