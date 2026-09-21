using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Exceptions
{
    public enum ErrorRetryability
    {
        Terminal,
        IdentityTerminal,
        Retryable
    }
}
