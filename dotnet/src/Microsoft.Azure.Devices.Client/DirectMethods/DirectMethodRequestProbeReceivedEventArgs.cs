using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodRequestProbeReceivedEventArgs : EventArgs
    {
        public string MethodName { get; init; }

        public uint ResponseTimeoutSeconds { get; init; }
    }
}
