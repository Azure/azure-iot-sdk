using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Retry
{
    public class RetryExpiredException : Exception
    {
        public RetryExpiredException(string message, Exception innerException) : base(message, innerException) { }
    }
}
