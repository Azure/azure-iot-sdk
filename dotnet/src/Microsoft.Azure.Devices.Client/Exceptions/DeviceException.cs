using System;
using System.Collections.Generic;
using System.Runtime.Serialization;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Exceptions
{
    public class DeviceException : Exception
    {
        public DeviceException()
        {
        }

        public DeviceException(string? message) : base(message)
        {
        }

        public DeviceException(string? message, Exception? innerException) : base(message, innerException)
        {
        }

        public required ErrorRetryability Retryability { get; set; }

        public required bool IsContained { get; set; }


        
    }
}
