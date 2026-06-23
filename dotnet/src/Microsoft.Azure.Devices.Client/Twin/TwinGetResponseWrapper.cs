using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Twin
{
    public class TwinGetResponseWrapper
    {
        public JsonObject? ReportedProperties { get; set; }

        public JsonObject? DesiredProperties { get; set; }

        public UInt64 ReportedPropertiesVersion { get; set; }

        public UInt64 DesiredPropertiesVersion { get; set; }
    }
}
