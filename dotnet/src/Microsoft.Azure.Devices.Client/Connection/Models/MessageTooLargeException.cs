using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Connection.Models
{
    public class MessageTooLargeException : Exception
    {
        public MessageTooLargeException(string message) : base(message)
        { 
        
        }
    }
}
