// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

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
