using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodResponse
    {
        public byte[]? Payload { get; set; }

        public required int Status { get; set; }
    }
}
