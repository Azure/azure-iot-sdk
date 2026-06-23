using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodRequestProbeReceivedEventArgs : EventArgs
    {
        public Probe Probe { get; set; }
    }
}
