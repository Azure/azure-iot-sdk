using Microsoft.Azure.Devices.Client.Exceptions;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public class DevicePresenceFlowCompletedArgs : EventArgs
    {
        public bool IsSuccess { get; set; }

        public DeviceException? Exception { get; set; }

        public DevicePresenceFlowCompletedArgs()
        {
            IsSuccess = true;
        }

        public DevicePresenceFlowCompletedArgs(DeviceException exception)
        {
            Exception = exception;
        }
    }
}
