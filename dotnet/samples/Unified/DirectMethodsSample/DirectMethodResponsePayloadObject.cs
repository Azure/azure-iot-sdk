using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Serialization;

namespace DirectMethodsClientSample
{
    public class DirectMethodResponsePayloadObject
    {
        [JsonPropertyName("someLongField")]
        public long SomeLongField { get; set; }

        [JsonPropertyName("someBooleanField")]
        public bool? SomeBooleanField { get; set; }
    }
}
