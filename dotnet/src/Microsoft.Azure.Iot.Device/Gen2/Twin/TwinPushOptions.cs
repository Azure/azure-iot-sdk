using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Gen2.Twin
{
    public class TwinPushOptions
    {
        /// <summary>
        /// If true, this device will receive the device's desired properties upon connecting (or re-connecting) to IoT hub. Additionally, while connected, this device will 
        /// receive desired property updates from IoT hub whenever they change.
        /// </summary>
        public bool ReceiveDesiredPropertyUpdates { get; set; } = true;

        /// <summary>
        /// If true, this device will receive the device's reported properties upon connecting (or re-connecting) to IoT hub.
        /// </summary>
        public bool ReceiveReportedPropertiesUponConnect { get; set; } = true;
    }
}
