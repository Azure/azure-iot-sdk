using Microsoft.Azure.Iot.Device.Exceptions;
using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Gen2.Connection
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
