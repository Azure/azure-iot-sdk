// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Provisioning.Models;
using System.Security.Cryptography.X509Certificates;

namespace SetupSampleDevice
{
    public class SampleConstants
    {
        // Every sample resolves the credential files against the samples root rather than the working directory, so the
        // files that SetupSampleDevice saves are the same ones the other samples load, however each is launched.
        private static readonly string outputPath = FindSamplesRoot();

        public static readonly string CertificatePath = outputPath + "certificate.cer";
        public static readonly string PfxPath = outputPath + "certificate.pfx";
        public const string TestCertificatesPassword = "Some dummy certificate password";

        private static string FindSamplesRoot()
        {
            foreach (string start in new[] { AppContext.BaseDirectory, Directory.GetCurrentDirectory() })
            {
                DirectoryInfo? directory = new(start);
                while (directory != null)
                {
                    if (File.Exists(Path.Combine(directory.FullName, "SetupSampleDevice", "SetupSampleDevice.csproj")))
                    {
                        return directory.FullName + Path.DirectorySeparatorChar;
                    }

                    directory = directory.Parent;
                }
            }

            throw new InvalidOperationException(
                "Could not locate the samples root directory (the one containing the SetupSampleDevice project).");
        }

        public static void ClearSavedCredentials()
        {
            try
            {
                File.Delete(outputPath + "deviceId.txt");
                File.Delete(outputPath + "idscope.txt");
                File.Delete(outputPath + "iothubhostname.txt");
                File.Delete(outputPath + "connectionprofile.txt");
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

        public static void SaveIotHubHostName(string iotHubHostName)
        {
            File.WriteAllText(outputPath + "iothubhostname.txt", iotHubHostName);
        }

        public static void SaveConnectionProfile(ConnectionProfile connectionProfile)
        {
            File.WriteAllText(outputPath + "connectionprofile.txt", connectionProfile.ToString());
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

        public static string? LoadIotHubHostName()
        {
            string? iotHubHostName = System.Environment.GetEnvironmentVariable("IOT_HUB_HOST_NAME");
            if (iotHubHostName != null)
            {
                return iotHubHostName;
            }

            if (!File.Exists(outputPath + "iothubhostname.txt"))
            {
                return null;
            }

            return File.ReadAllText(outputPath + "iothubhostname.txt");
        }

        public static ConnectionProfile LoadConnectionProfile()
        {
            string? connectionProfile = System.Environment.GetEnvironmentVariable("IOT_HUB_CONNECTION_PROFILE");
            if (string.IsNullOrEmpty(connectionProfile) && File.Exists(outputPath + "connectionprofile.txt"))
            {
                connectionProfile = File.ReadAllText(outputPath + "connectionprofile.txt");
            }

            // Default to the classic profile when no profile was persisted, matching the enum's default.
            if (string.IsNullOrEmpty(connectionProfile))
            {
                return ConnectionProfile.Classic;
            }

            return Enum.Parse<ConnectionProfile>(connectionProfile, ignoreCase: true);
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
