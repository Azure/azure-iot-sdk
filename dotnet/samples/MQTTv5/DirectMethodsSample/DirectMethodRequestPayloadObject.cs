// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

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
