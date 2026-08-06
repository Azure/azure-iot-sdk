using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.DirectMethods.Gen2
{
    public class DirectMethodRequestProbeReceivedEventArgs : EventArgs
    {
        public required string MethodName { get; init; }

        public required uint ResponseTimeoutSeconds { get; init; }
    }
}
