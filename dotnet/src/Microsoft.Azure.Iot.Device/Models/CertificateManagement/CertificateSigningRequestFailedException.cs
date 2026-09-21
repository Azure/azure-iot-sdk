using System;
using System.Collections.Generic;
using System.Text;

namespace Microsoft.Azure.Iot.Device.Models.CertificateManagement
{
    public class CertificateSigningRequestFailedException : Exception
    {
        public required CertificateSigningRequestErrorResponse Error { get; set; }
    }
}
