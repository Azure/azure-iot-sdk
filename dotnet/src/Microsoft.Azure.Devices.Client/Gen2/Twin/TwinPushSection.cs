using System.Text.Json;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Gen2.Twin
{
    public class TwinPushSection
    {
        public JsonObject Properties { get; set; }

        public ulong PropertiesVersion { get; set; }
    }
}
