using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public class DevicePresenceFlowCompletedArgs : EventArgs
    {
        public bool IsSuccess { get; set; }

        public ConnectBirthException? Exception { get; set; }
    }
}
