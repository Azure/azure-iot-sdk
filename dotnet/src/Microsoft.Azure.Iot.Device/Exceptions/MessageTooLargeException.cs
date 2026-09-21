using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Exceptions
{
    public class MessageTooLargeException : Exception
    {
        public MessageTooLargeException(string message) : base(message)
        { 
        
        }
    }
}
