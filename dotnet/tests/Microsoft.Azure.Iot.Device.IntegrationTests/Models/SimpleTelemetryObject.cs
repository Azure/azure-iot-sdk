using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Serialization;

namespace Microsoft.Azure.Iot.Device.IntegrationTests.Models
{
    public class SimpleTelemetryObject
    {
        [JsonPropertyName("SomeString")]
        public string? SomeString { get; set; }
    }
}
