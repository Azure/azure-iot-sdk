using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Gen2.Twin
{
    public class ReportedPatchRequest
    {
        public required JsonObject ReportedProperties { get; set; }

        public UInt64 IfMatch { get; set; }
    }
}
