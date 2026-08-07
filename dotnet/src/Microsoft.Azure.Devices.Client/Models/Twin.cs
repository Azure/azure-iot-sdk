using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Models
{
    public class Twin
    {
        public JsonObject? Desired { get; set; }

        public JsonObject? Reported { get; set; }

        public UInt64? DesiredVersion { get; set; }

        public UInt64? ReportedVersion { get; set; }
    }
}
