using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public class ConnectBirthException : Exception
    {
        public ConnectBirthException(string message) : base(message) { }

        public ConnectBirthException(string message, Exception e) : base(message, e) { }

    }
}
