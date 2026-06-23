using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Twin
{
    public class DesiredPropertyUpdateReceivedEventArgs : EventArgs
    {
        public ulong DesiredPropertiesVersion { get; set; }

        public JsonObject DesiredProperties { get; set; }
    }
}
