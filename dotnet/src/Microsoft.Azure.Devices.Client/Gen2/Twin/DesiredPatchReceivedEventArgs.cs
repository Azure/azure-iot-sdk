using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Gen2.Twin
{
    public class DesiredPatchReceivedEventArgs : EventArgs
    {
        public required ulong DesiredPropertiesVersion { get; set; }

        public required JsonObject DesiredProperties { get; set; }
    }
}
