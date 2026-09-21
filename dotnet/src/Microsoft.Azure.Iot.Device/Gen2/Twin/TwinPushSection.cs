using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Iot.Device.Gen2.Twin
{
    public class TwinPushSection
    {
        public required JsonObject Properties { get; set; }

        public required ulong PropertiesVersion { get; set; }
    }
}
