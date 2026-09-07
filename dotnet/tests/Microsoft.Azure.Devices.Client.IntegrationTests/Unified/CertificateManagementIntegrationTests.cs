using Microsoft.Azure.Devices.Client.Models.CertificateManagement;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using System.Security.Cryptography.X509Certificates;
using Xunit;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.Unified
{
    public class CertificateManagementIntegrationTests
    {

        //Fixed in a different PR

        private static X509Certificate2 CreateX509CertificateFromKeyAndCert(string certificate, string key)
        {
            // Create X509Certificate2 from PEM
            using var cert = X509Certificate2.CreateFromPem(certificate, key);

            // Note: On Windows, we need to export and reimport to allow ephemeral key use
            byte[] certificateBytes = cert.Export(X509ContentType.Pfx);
            using var exportedCert = X509CertificateLoader.LoadCertificate(certificateBytes);

            return exportedCert;
        }

        private static string CertificateListToPem(IReadOnlyList<string> certList)
        {
            const string beginHeader = "-----BEGIN CERTIFICATE-----\r\n";
            const string endFooter = "\r\n-----END CERTIFICATE-----";
            string separator = endFooter + "\r\n" + beginHeader;
            return beginHeader + string.Join(separator, certList) + endFooter;
        }
    }
}
