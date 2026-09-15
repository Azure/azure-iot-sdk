using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Serialization;

namespace DirectMethodsClientSample
{
    public class DirectMethodRequestPayloadObject
    {
        [JsonPropertyName("someStringField")]
        public string? SomeStringField { get; set; }

        [JsonPropertyName("someIntegerField")]
        public int? SomeIntegerField { get; set; }
    }
}
