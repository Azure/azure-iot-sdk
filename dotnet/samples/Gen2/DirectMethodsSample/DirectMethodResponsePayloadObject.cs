// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

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
