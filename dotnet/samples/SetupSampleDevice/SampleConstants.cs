using Microsoft.Azure.Iot.Device;
using System.Security.Cryptography.X509Certificates;

namespace SetupSampleDevice
{
    public class SampleConstants
    {
        public const string CertificatePath = outputPath + $"certificate.cer";
        public const string PfxPath = outputPath + $"certificate.pfx";
        public const string TestCertificatesPassword = "Some dummy certificate password";

        private const string outputPath = "../../../../../../";

        public static void ClearSavedCredentials()
        {
            try
            {
                File.Delete(outputPath + "deviceId.txt");
                File.Delete(outputPath + "idscope.txt");
                File.Delete(outputPath + "certificate.cer");
                File.Delete(outputPath + "certificate.pfx");
            }
            catch (DirectoryNotFoundException)
            { 
                // sample credentials are already deleted
            }
        }

        public static void SaveDeviceId(string deviceId)
        {
            File.WriteAllText(outputPath + "deviceId.txt", deviceId);
        }

        public static void SaveIdScope(string idScope)
        {
            File.WriteAllText(outputPath + "idscope.txt", idScope);
        }

        public static void SaveX509(byte[] pfx, byte[] certificate)
        {
            File.WriteAllBytes(outputPath + "certificate.pfx", pfx);
            File.WriteAllBytes(outputPath + "certificate.cer", certificate);
        }

        public static string LoadDeviceId()
        {
            string? deviceId = System.Environment.GetEnvironmentVariable("SAMPLE_DEVICE_ID");
            if (deviceId != null)
            {
                return deviceId;
            }

            if (!File.Exists(outputPath + "deviceId.txt"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllText(outputPath + "deviceId.txt");
        }

        public static string LoadIdScope()
        {
            string? idScope = System.Environment.GetEnvironmentVariable("DPS_ID_SCOPE");
            if (idScope != null)
            {
                return idScope;
            }

            if (!File.Exists(outputPath + "idscope.txt"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllText(outputPath + "idscope.txt");
        }

        public static byte[] LoadPfx()
        {
            if (!File.Exists(outputPath + "certificate.pfx"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllBytes(outputPath + "certificate.pfx");
        }

        public static X509AuthenticationProvider LoadAuthenticationProvider()
        {
            X509Certificate2 certificate;
            string? certPassword = System.Environment.GetEnvironmentVariable("_PASSWORD");
            if (string.IsNullOrEmpty(certPassword))
            {
                certPassword = SampleConstants.TestCertificatesPassword;
            }

            string? pfxContentsPath = System.Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH");

            if (!string.IsNullOrEmpty(pfxContentsPath) && File.Exists(pfxContentsPath))
            {
                certificate = X509CertificateLoader.LoadPkcs12FromFile(pfxContentsPath, certPassword);
            }
            else 
            {
                certificate = X509CertificateLoader.LoadPkcs12(SampleConstants.LoadPfx(), certPassword);
            }

            return new(certificate);
        }
    }
}
