using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodRequestReceivedEventArgs : EventArgs
    {
        public byte[]? Payload { get; init; }

        public required string MethodName { get; init; }
    }
}
