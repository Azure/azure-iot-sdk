using Microsoft.Azure.Devices;
using Microsoft.Azure.Devices.Client;
using System;
using System.Collections.Generic;
using System.Runtime.ConstrainedExecution;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace SetupSampleDevice
{
    public class SampleConstants
    {
        public const string CertificatePath = $"../../../../certificate.cer";
        public const string PfxPath = $"../../../../certificate.pfx";
        public const string TestCertificatesPassword = "Some dummy certificate password";

        private const string outputPath = "../../../../";

        public static void ClearSavedCredentials()
        {
            try
            {
                File.Delete(outputPath + "deviceId.txt");
                File.Delete(outputPath + "hostname.txt");
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

        public static void SaveHostname(string hostname)
        {
            File.WriteAllText(outputPath + "hostname.txt", hostname);
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
            if (!File.Exists(outputPath + "deviceId.txt"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllText(outputPath + "deviceId.txt");
        }

        public static string LoadHostname()
        {
            if (!File.Exists(outputPath + "deviceId.txt"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllText(outputPath + "hostname.txt");
        }

        public static string LoadIdScope()
        {
            if (!File.Exists(outputPath + "deviceId.txt"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllText(outputPath + "idscope.txt");
        }

        public static byte[] LoadPfx()
        {
            if (!File.Exists(outputPath + "deviceId.txt"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllBytes(outputPath + "certificate.pfx");
        }

        public static X509AuthenticationProvider LoadAuthenticationProvider()
        {
            return new(X509CertificateLoader.LoadPkcs12(SampleConstants.LoadPfx(), SampleConstants.TestCertificatesPassword));
        }
    }
}
