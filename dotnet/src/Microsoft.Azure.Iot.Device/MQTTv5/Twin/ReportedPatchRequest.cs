// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.MQTTv5.Twin
{
    public class ReportedPatchRequest
    {
        public required JsonObject ReportedProperties { get; set; }

        public UInt64 IfMatch { get; set; }
    }
}
