using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Unified.Connection.Retry
{
    public class RetryExpiredException : Exception
    {
        public RetryExpiredException(string message, Exception innerException) : base(message, innerException) { }
    }
}
