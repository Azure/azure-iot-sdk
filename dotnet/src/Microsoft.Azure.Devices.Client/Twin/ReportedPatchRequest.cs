using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Twin
{
    public class ReportedPatchRequest
    {
        public JsonObject ReportedProperties { get; set; }

        public UInt64 IfMatch { get; set; }
    }
}
