using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace Microsoft.Azure.Devices.Client.IntegrationTests
{
    /// <summary>
    /// Provides helper methods for working with certificates issued by IoT Hub or DPS.
    /// </summary>
    public static class CertificateUtilities
    {
        private const string BeginCertificate = "-----BEGIN CERTIFICATE-----";
        private const string EndCertificate = "-----END CERTIFICATE-----";

        /// <summary>
        /// Converts a list of base64-encoded certificates to PEM format.
        /// </summary>
        /// <param name="certificateChain">Array of base64-encoded certificates.</param>
        /// <returns>PEM-formatted certificate chain string.</returns>
        /// <exception cref="ArgumentNullException">Thrown when <paramref name="certificateChain"/> is null.</exception>
        public static string ConvertToPem(IReadOnlyList<string> certificateChain)
        {
            if (certificateChain == null)
            {
                throw new ArgumentNullException(nameof(certificateChain));
            }

            var sb = new StringBuilder();
            foreach (string cert in certificateChain)
            {
                sb.AppendLine(BeginCertificate);
                sb.AppendLine(cert);
                sb.AppendLine(EndCertificate);
            }
            return sb.ToString();
        }

        /// <summary>
        /// Creates an X509Certificate2 with private key from the issued certificate chain.
        /// </summary>
        /// <param name="certificateChain">Issued certificate chain. The first element should be the leaf/device certificate.</param>
        /// <param name="privateKey">The RSA private key corresponding to the CSR.</param>
        /// <returns>X509Certificate2 with private key for IoT Hub authentication.</returns>
        /// <exception cref="ArgumentNullException">Thrown when <paramref name="certificateChain"/> or <paramref name="privateKey"/> is null.</exception>
        /// <exception cref="ArgumentException">Thrown when <paramref name="certificateChain"/> is empty.</exception>
        public static X509Certificate2 CreateCertificateWithPrivateKey(
            IReadOnlyList<string> certificateChain,
            RSA privateKey)
        {
            if (certificateChain == null)
            {
                throw new ArgumentNullException(nameof(certificateChain));
            }

            if (privateKey == null)
            {
                throw new ArgumentNullException(nameof(privateKey));
            }

            if (certificateChain.Count == 0)
            {
                throw new ArgumentException("Certificate chain cannot be empty.", nameof(certificateChain));
            }

            byte[] leafCertBytes = Convert.FromBase64String(certificateChain[0]);
            using var leafCert = X509CertificateLoader.LoadCertificate(leafCertBytes);
            return leafCert.CopyWithPrivateKey(privateKey);
        }

        /// <summary>
        /// Creates an X509Certificate2 with private key from the issued certificate chain.
        /// </summary>
        /// <param name="certificateChain">Issued certificate chain. The first element should be the leaf/device certificate.</param>
        /// <param name="privateKey">The ECDsa private key corresponding to the CSR.</param>
        /// <returns>X509Certificate2 with private key for IoT Hub authentication.</returns>
        /// <exception cref="ArgumentNullException">Thrown when <paramref name="certificateChain"/> or <paramref name="privateKey"/> is null.</exception>
        /// <exception cref="ArgumentException">Thrown when <paramref name="certificateChain"/> is empty.</exception>
        public static X509Certificate2 CreateCertificateWithPrivateKey(
            IReadOnlyList<string> certificateChain,
            ECDsa privateKey)
        {
            if (certificateChain == null)
            {
                throw new ArgumentNullException(nameof(certificateChain));
            }

            if (privateKey == null)
            {
                throw new ArgumentNullException(nameof(privateKey));
            }

            if (certificateChain.Count == 0)
            {
                throw new ArgumentException("Certificate chain cannot be empty.", nameof(certificateChain));
            }

            byte[] leafCertBytes = Convert.FromBase64String(certificateChain[0]);
            using var leafCert = X509CertificateLoader.LoadCertificate(leafCertBytes);
            return leafCert.CopyWithPrivateKey(privateKey);
        }

        public static X509Certificate2 CreateCertificateWithPrivateKey(
            System.Collections.Generic.IReadOnlyList<string> certificateChain,
            AsymmetricAlgorithm privateKey)
        {
            return privateKey switch
            {
                ECDsa ecdsa => CreateCertificateWithPrivateKey(certificateChain, ecdsa),
                RSA rsa => CreateCertificateWithPrivateKey(certificateChain, rsa),
                _ => throw new NotSupportedException($"Unsupported key type: {privateKey.GetType()}")
            };
        }

        public static (string csrBase64, AsymmetricAlgorithm privateKey) GenerateCsrAndPrivateKey(string registrationId, CsrAlgorithm csrAlgorithm)
        {
            if (csrAlgorithm == CsrAlgorithm.ECC)
            {
                var ecdsa = ECDsa.Create(ECCurve.NamedCurves.nistP256);
                var request = new CertificateRequest(
                    $"CN={registrationId}",
                    ecdsa,
                    HashAlgorithmName.SHA256);

                byte[] csrDer = request.CreateSigningRequest();
                return (Convert.ToBase64String(csrDer), ecdsa);
            }
            else
            {
                var rsa = RSA.Create(2048);
                var request = new CertificateRequest(
                    $"CN={registrationId}",
                    rsa,
                    HashAlgorithmName.SHA256,
                    RSASignaturePadding.Pkcs1);

                byte[] csrDer = request.CreateSigningRequest();
                return (Convert.ToBase64String(csrDer), rsa);
            }
        }

        public static string GenerateCsrWithPrivateKey(string registrationId, AsymmetricAlgorithm privateKey)
        {
            if (privateKey is ECDsa eccPrivateKey)
            {
                var request = new CertificateRequest(
                    $"CN={registrationId}",
                    eccPrivateKey,
                    HashAlgorithmName.SHA256);

                byte[] csrDer = request.CreateSigningRequest();
                return Convert.ToBase64String(csrDer);
            }
            else if (privateKey is RSA rsaPrivateKey)
            {
                var request = new CertificateRequest(
                    $"CN={registrationId}",
                    rsaPrivateKey,
                    HashAlgorithmName.SHA256,
                    RSASignaturePadding.Pkcs1);

                byte[] csrDer = request.CreateSigningRequest();
                return Convert.ToBase64String(csrDer);
            }
            else
            {
                throw new NotSupportedException("Unrecognized private key");
            }
        }

        public enum CsrAlgorithm
        {
            ECC,
            RSA,
        }
    }
}