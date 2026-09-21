using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models.DirectMethods
{
    public class DirectMethodRequestReceivedEventArgs : EventArgs
    {
        public byte[]? Payload { get; init; }

        public required string MethodName { get; init; }
    }
}
