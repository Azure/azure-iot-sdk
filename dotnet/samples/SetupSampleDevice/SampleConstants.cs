// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Authentication.Boot;
using Microsoft.Azure.Iot.Device.Authentication.Operational;
using Microsoft.Azure.Iot.Device.Models;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace SetupSampleDevice
{
    public class SampleConstants
    {
        // Every sample resolves the credential files against the samples root rather than the working directory, so the
        // files that SetupSampleDevice saves are the same ones the other samples load, however each is launched.
        private static readonly string outputPath = FindSamplesRoot();

        public static readonly string X509CertificatePath = outputPath + "certificate.cer";
        public static readonly string X509PfxPath = outputPath + "certificate.pfx";
        public const string X509CertificatePassword = "Some dummy certificate password";

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
                File.Delete(outputPath + "x509DeviceId.txt");
                File.Delete(outputPath + "idscope.txt");
                File.Delete(outputPath + "iothubhostname.txt");
                File.Delete(outputPath + "connectionprofile.txt");
                File.Delete(outputPath + "certificate.cer");
                File.Delete(outputPath + "certificate.pfx");
                File.Delete(outputPath + "symmetricKeyDeviceId.txt");
                File.Delete(outputPath + "symmetricKey.txt");
                File.Delete(outputPath + "symmetricKeyDevicePrivateKey.pem");
            }
            catch (DirectoryNotFoundException)
            {
                // sample credentials are already deleted
            }
        }

        public static void SaveX509DeviceId(string deviceId)
        {
            File.WriteAllText(outputPath + "x509DeviceId.txt", deviceId);
        }

        public static void SaveIdScope(string idScope)
        {
            File.WriteAllText(outputPath + "idscope.txt", idScope);
        }

        public static void SaveIotHubHostName(string iotHubHostName)
        {
            File.WriteAllText(outputPath + "iothubhostname.txt", iotHubHostName);
        }

        public static void SaveConnectionProfile(IotHubConnectionType connectionProfile)
        {
            File.WriteAllText(outputPath + "connectionprofile.txt", connectionProfile.ToString());
        }

        public static void SaveX509Certificate(byte[] pfx, byte[] certificate)
        {
            File.WriteAllBytes(outputPath + "certificate.pfx", pfx);
            File.WriteAllBytes(outputPath + "certificate.cer", certificate);
        }

        public static void SaveSymmetricKeyDeviceId(string deviceId)
        {
            File.WriteAllText(outputPath + "symmetricKeyDeviceId.txt", deviceId);
        }

        public static void SaveSymmetricKey(string symmetricKey)
        {
            File.WriteAllText(outputPath + "symmetricKey.txt", symmetricKey);
        }

        /// <summary>
        /// Saves the private key that the symmetric key sample device signs its certificate signing requests with, as a
        /// PKCS#8 PEM.
        /// </summary>
        public static void SaveSymmetricKeyDevicePrivateKey(AsymmetricAlgorithm privateKey)
        {
            File.WriteAllText(outputPath + "symmetricKeyDevicePrivateKey.pem", privateKey.ExportPkcs8PrivateKeyPem());
        }

        /// <summary>
        /// Loads the private key that the symmetric key sample device signs its certificate signing requests with. The
        /// symmetric key authenticates the device to DPS; this key pairs with the certificates DPS and IoT Hub issue to it.
        /// </summary>
        public static AsymmetricAlgorithm LoadSymmetricKeyDevicePrivateKey()
        {
            string privateKeyPem = LoadValue("SAMPLE_SYMMETRIC_KEY_DEVICE_PRIVATE_KEY", "symmetricKeyDevicePrivateKey.pem");

            // A PKCS#8 PEM doesn't name its algorithm in the label, so try each supported one in turn.
            RSA rsa = RSA.Create();
            try
            {
                rsa.ImportFromPem(privateKeyPem);
                return rsa;
            }
            catch (CryptographicException)
            {
                rsa.Dispose();
            }

            ECDsa ecdsa = ECDsa.Create();
            try
            {
                ecdsa.ImportFromPem(privateKeyPem);
                return ecdsa;
            }
            catch (CryptographicException)
            {
                ecdsa.Dispose();
                throw new InvalidOperationException("The symmetric key sample device's private key is not an RSA or ECDsa private key.");
            }
        }

        /// <summary>
        /// Loads the Id of the sample device that authenticates with a symmetric key. This is also its DPS registration Id.
        /// </summary>
        public static string LoadSymmetricKeyDeviceId()
        {
            return LoadValue("SAMPLE_SYMMETRIC_KEY_DEVICE_ID", "symmetricKeyDeviceId.txt");
        }

        /// <summary>
        /// Loads the Base64-encoded symmetric key of the sample device that authenticates with a symmetric key.
        /// </summary>
        public static string LoadSymmetricKey()
        {
            return LoadValue("SAMPLE_SYMMETRIC_KEY", "symmetricKey.txt");
        }

        private static string LoadValue(string environmentVariableName, string fileName)
        {
            string? value = System.Environment.GetEnvironmentVariable(environmentVariableName);
            if (value != null)
            {
                return value;
            }

            if (!File.Exists(outputPath + fileName))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllText(outputPath + fileName);
        }

        public static string LoadX509DeviceId()
        {
            return LoadValue("SAMPLE_X509_DEVICE_ID", "x509DeviceId.txt");
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

        public static IotHubConnectionType LoadConnectionProfile()
        {
            string? connectionProfile = System.Environment.GetEnvironmentVariable("IOT_HUB_CONNECTION_PROFILE");
            if (string.IsNullOrEmpty(connectionProfile) && File.Exists(outputPath + "connectionprofile.txt"))
            {
                connectionProfile = File.ReadAllText(outputPath + "connectionprofile.txt");
            }

            // Default to the MQTT 3.x profile when no profile was persisted, matching the enum's default.
            if (string.IsNullOrEmpty(connectionProfile))
            {
                return IotHubConnectionType.Mqttv3;
            }

            return Enum.Parse<IotHubConnectionType>(connectionProfile, ignoreCase: true);
        }

        public static byte[] LoadX509Pfx()
        {
            if (!File.Exists(outputPath + "certificate.pfx"))
            {
                throw new Exception("Run the SetupSampleDevice sample first to generate a test device + credentials");
            }

            return File.ReadAllBytes(outputPath + "certificate.pfx");
        }

        public static X509Certificate2 LoadX509BootCertificate()
        {
            X509Certificate2 certificate;
            string? certPassword = System.Environment.GetEnvironmentVariable("_PASSWORD");
            if (string.IsNullOrEmpty(certPassword))
            {
                certPassword = SampleConstants.X509CertificatePassword;
            }

            string? pfxContentsPath = System.Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH");

            if (!string.IsNullOrEmpty(pfxContentsPath) && File.Exists(pfxContentsPath))
            {
                certificate = X509CertificateLoader.LoadPkcs12FromFile(pfxContentsPath, certPassword);
            }
            else
            {
                certificate = X509CertificateLoader.LoadPkcs12(SampleConstants.LoadX509Pfx(), certPassword);
            }

            return certificate;
        }

        /// <summary>
        /// Loads the sample device's private key from its X.509 certificate.
        /// </summary>
        public static AsymmetricAlgorithm LoadX509PrivateKey()
        {
            // The certificate isn't disposed because some platforms tie the key's lifetime to the certificate it came from.
            X509Certificate2 certificate = LoadX509BootCertificate();
            return (AsymmetricAlgorithm?)certificate.GetRSAPrivateKey()
                ?? certificate.GetECDsaPrivateKey()
                ?? throw new InvalidOperationException("The sample device certificate has no RSA or ECDsa private key.");
        }

        public static X509BootAuthenticationProvider LoadX509BootAuthenticationProvider()
        {
            return new(LoadX509BootCertificate());
        }

        public static X509OperationalAuthenticationProvider LoadX509OperationalAuthenticationProvider()
        {
            return new(LoadX509BootCertificate());
        }

        public static SymmetricKeyBootAuthenticationProvider LoadSymmetricKeyBootAuthenticationProvider()
        {
            return new(LoadSymmetricKeyDeviceId(), LoadIdScope(), LoadSymmetricKey());
        }

        public static SymmetricKeyOperationalAuthenticationProvider LoadSymmetricKeyOperationalAuthenticationProvider()
        {
            return new(LoadSymmetricKey());
        }
    }
}
