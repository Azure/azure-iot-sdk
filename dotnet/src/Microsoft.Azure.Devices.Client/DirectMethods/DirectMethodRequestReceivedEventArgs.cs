using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.DirectMethods
{
    public class DirectMethodRequestReceivedEventArgs : EventArgs
    {
        public DirectMethodRequest Request { get; set; }
    }
}
