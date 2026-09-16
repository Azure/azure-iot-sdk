using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Exceptions
{
    public enum ErrorRetryability
    {
        Terminal,
        IdentityTerminal,
        Retryable
    }
}
