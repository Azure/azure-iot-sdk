using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Devices.Client.CertificateManagement
{
    public class CertificateSigningRequestFailedException : Exception
    {
        public required CertificateSigningRequestErrorResponse Error { get; set; }
    }
}
