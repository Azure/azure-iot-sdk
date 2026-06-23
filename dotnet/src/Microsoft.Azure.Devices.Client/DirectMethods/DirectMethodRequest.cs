using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodRequest
    {
        public byte[] Payload { get; internal set; }

        public string MethodName { get; internal set; }

        internal string RequestId { get; set; }
    }
}
