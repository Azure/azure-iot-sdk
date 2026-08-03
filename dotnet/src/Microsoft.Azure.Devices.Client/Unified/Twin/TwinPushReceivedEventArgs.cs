using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;

namespace Microsoft.Azure.Devices.Client.Unified.Twin
{
    public class TwinPushReceivedEventArgs : EventArgs
    {
        public TwinPushSection? Desired { get; set; }

        public TwinPushSection? Reported { get; set; }
    }
}
