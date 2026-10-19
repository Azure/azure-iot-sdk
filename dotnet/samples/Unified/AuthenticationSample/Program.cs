// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.CertificateManagement;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using SetupSampleDevice;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

/// <summary>
/// Shows each way a device can authenticate when it boots (with the device provisioning service) and when it operates
/// (with IoT Hub). Every scenario where IoT Hub authenticates the device with an issued certificate also renews that
/// certificate by sending a certificate signing request to IoT Hub, then reconnects with the certificate IoT Hub issued.
/// </summary>
internal class Program
{
    private static async Task Main()
    {
        using CancellationTokenSource cts = new();
        cts.CancelAfter(TimeSpan.FromMinutes(2));

        // Cancel sample on key press
        Console.CancelKeyPress += (sender, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };

        Console.WriteLine("Running scenario 'x509': X.509 certificate to DPS, the same X.509 certificate to IoT Hub");
        await X509BootAndOperationalAsync(cts.Token);

        Console.WriteLine("Running scenario 'x509-issued': X.509 certificate to DPS, DPS-issued certificate to IoT Hub, renewed by IoT Hub");
        await X509BootIssuedOperationalAsync(cts.Token);

        Console.WriteLine("Running scenario 'x509-issued-callback': As x509-issued, but the application prepares each issued certificate in a callback");
        await X509BootIssuedOperationalWithCallbackAsync(cts.Token);

        Console.WriteLine("Running scenario 'sas': Symmetric key (SAS token) to DPS, symmetric key (SAS token) to IoT Hub");
        await SymmetricKeyBootAndOperationalAsync(cts.Token);

        Console.WriteLine("Running scenario 'sas-issued': Symmetric key (SAS token) to DPS, DPS-issued certificate to IoT Hub, renewed by IoT Hub");
        await SymmetricKeyBootIssuedOperationalAsync(cts.Token);

        Console.WriteLine("Running scenario 'sas-issued-callback': As sas-issued, but the application prepares each issued certificate in a callback");
        await SymmetricKeyBootIssuedOperationalWithCallbackAsync(cts.Token);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // X.509 certificate at boot
    // ---------------------------------------------------------------------------------------------------------------

    /// <summary>
    /// The device presents its X.509 certificate to DPS and then the same certificate to IoT Hub.
    /// </summary>
    private static async Task X509BootAndOperationalAsync(CancellationToken cancellationToken)
    {
        X509AuthenticationProvider authentication = new(SampleConstants.LoadBootCertificate());
        ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope());

        using ConnectionClient connectionClient = new();
        ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cancellationToken);
        Console.WriteLine($"Device {connectionContext.DeviceId} connected to {connectionContext.IotHubHostName} with its X.509 certificate");

        await connectionClient.DisconnectAsync(cancellationToken);
    }

    /// <summary>
    /// The device presents its X.509 certificate to DPS along with a certificate signing request. DPS issues an operational
    /// certificate, which the SDK pairs with the certificate signing request's private key and presents to IoT Hub.
    /// </summary>
    private static async Task X509BootIssuedOperationalAsync(CancellationToken cancellationToken)
    {
        // The private key that DPS and IoT Hub issue operational certificates for. In production, keep it in hardware
        // (an HSM or TPM); the SDK only asks it to sign and never exports it.
        using ECDsa operationalKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);

        // Passing the operational key tells the provider to present issued certificates, paired with that key, to IoT Hub
        // instead of the boot certificate. Every certificate signing request this device sends must use this same key.
        X509AuthenticationProvider authentication = new(SampleConstants.LoadBootCertificate(), operationalKey);

        ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope())
        {
            // The certificate signing request's subject must be the registration Id.
            CertificateSigningRequest = new(operationalKey, CreateCertificateSigningRequest(authentication.GetRegistrationId(), operationalKey)),
        };

        await ProvisionWithIssuanceThenRenewWithIotHubAsync(provisioningSettings, authentication, () => operationalKey, "its X.509 certificate", cancellationToken);
    }

    /// <summary>
    /// As <see cref="X509BootIssuedOperationalAsync"/>, but the application turns each issued certificate chain into the
    /// certificate to present to IoT Hub in a callback. This lets it persist the chain, pair it with a key the SDK never
    /// sees (for example one in an HSM), or rotate to a new key for each certificate signing request.
    /// </summary>
    private static async Task X509BootIssuedOperationalWithCallbackAsync(CancellationToken cancellationToken)
    {
        using OperationalCredentialStore credentialStore = new();
        ECDsa operationalKey = credentialStore.CreateKey();

        X509AuthenticationProvider authentication = new(SampleConstants.LoadBootCertificate(), credentialStore.HandleCertificateSigningCompleteAsync);

        ProvisioningSettings provisioningSettings = new(SampleConstants.LoadIdScope())
        {
            CertificateSigningRequest = new(operationalKey, CreateCertificateSigningRequest(authentication.GetRegistrationId(), operationalKey)),
        };

        // The callback finds the private key for whichever certificate it is given, so this device can use a new key for
        // its IoT Hub certificate signing request.
        await ProvisionWithIssuanceThenRenewWithIotHubAsync(provisioningSettings, authentication, credentialStore.CreateKey, "its X.509 certificate", cancellationToken);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Symmetric key at boot
    // ---------------------------------------------------------------------------------------------------------------

    /// <summary>
    /// The device signs SAS tokens with its symmetric key to authenticate with DPS and then with IoT Hub.
    /// </summary>
    private static async Task SymmetricKeyBootAndOperationalAsync(CancellationToken cancellationToken)
    {
        string idScope = SampleConstants.LoadIdScope();
        SymmetricKeyAuthenticationProvider authentication = new(
            SampleConstants.LoadSymmetricKeyRegistrationId(),
            idScope,
            SampleConstants.LoadSymmetricKey());

        ProvisioningSettings provisioningSettings = new(idScope);

        using ConnectionClient connectionClient = new();
        ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cancellationToken);
        Console.WriteLine($"Device {connectionContext.DeviceId} connected to {connectionContext.IotHubHostName} with a SAS token signed with its symmetric key");

        await connectionClient.DisconnectAsync(cancellationToken);
    }

    /// <summary>
    /// The device authenticates with DPS with a SAS token and sends a certificate signing request. DPS issues an
    /// operational certificate, which the SDK pairs with the certificate signing request's private key and presents to
    /// IoT Hub instead of a SAS token.
    /// </summary>
    private static async Task SymmetricKeyBootIssuedOperationalAsync(CancellationToken cancellationToken)
    {
        using ECDsa operationalKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);

        string idScope = SampleConstants.LoadIdScope();
        string registrationId = SampleConstants.LoadSymmetricKeyRegistrationId();

        // Until a certificate is issued, this provider authenticates with IoT Hub with a SAS token
        SymmetricKeyAuthenticationProvider authentication = new(registrationId, idScope, SampleConstants.LoadSymmetricKey(), operationalKey);

        ProvisioningSettings provisioningSettings = new(idScope)
        {
            CertificateSigningRequest = new(operationalKey, CreateCertificateSigningRequest(registrationId, operationalKey)),
        };

        await ProvisionWithIssuanceThenRenewWithIotHubAsync(provisioningSettings, authentication, () => operationalKey, "a SAS token signed with its symmetric key", cancellationToken);
    }

    /// <summary>
    /// As <see cref="SymmetricKeyBootIssuedOperationalAsync"/>, but the application prepares each issued certificate in
    /// a callback and uses a new key for its IoT Hub certificate signing request.
    /// </summary>
    private static async Task SymmetricKeyBootIssuedOperationalWithCallbackAsync(CancellationToken cancellationToken)
    {
        using OperationalCredentialStore credentialStore = new();
        ECDsa operationalKey = credentialStore.CreateKey();

        string idScope = SampleConstants.LoadIdScope();
        string registrationId = SampleConstants.LoadSymmetricKeyRegistrationId();

        SymmetricKeyAuthenticationProvider authentication = new(
            registrationId,
            idScope,
            SampleConstants.LoadSymmetricKey(),
            credentialStore.HandleCertificateSigningCompleteAsync);

        ProvisioningSettings provisioningSettings = new(idScope)
        {
            CertificateSigningRequest = new(operationalKey, CreateCertificateSigningRequest(registrationId, operationalKey)),
        };

        await ProvisionWithIssuanceThenRenewWithIotHubAsync(provisioningSettings, authentication, credentialStore.CreateKey, "a SAS token signed with its symmetric key", cancellationToken);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Shared flow for the scenarios where IoT Hub authenticates the device with an issued certificate
    // ---------------------------------------------------------------------------------------------------------------

    /// <summary>
    /// Provisions with a certificate signing request and connects to IoT Hub with the certificate DPS issued. Then asks
    /// IoT Hub for a new certificate and reconnects with it.
    /// </summary>
    /// <param name="getIotHubCertificateSigningKey">Returns the private key to generate the IoT Hub certificate signing request with.</param>
    /// <param name="fallbackCredential">What the device authenticates with IoT Hub with when no certificate is issued.</param>
    private static async Task ProvisionWithIssuanceThenRenewWithIotHubAsync(
        ProvisioningSettings provisioningSettings,
        AuthenticationProvider authentication,
        Func<ECDsa> getIotHubCertificateSigningKey,
        string fallbackCredential,
        CancellationToken cancellationToken)
    {
        ConnectionContext connectionContext;
        IReadOnlyList<string>? iotHubIssuedCertificates;

        using (ConnectionClient connectionClient = new())
        {
            connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cancellationToken);

            if (connectionContext.IssuedClientCertificates is not { Count: > 0 })
            {
                Console.WriteLine($"Device {connectionContext.DeviceId} connected to {connectionContext.IotHubHostName} with {fallbackCredential}");
                Console.WriteLine(
                    "DPS did not issue a certificate, so this device cannot authenticate with an issued certificate. Link this " +
                    "device's enrollment to a certificate management credential policy, then run this scenario again.");
                await connectionClient.DisconnectAsync(cancellationToken);
                return;
            }

            Console.WriteLine($"Device {connectionContext.DeviceId} connected to {connectionContext.IotHubHostName} with the certificate DPS issued: {DescribeCertificateChain(connectionContext.IssuedClientCertificates)}");

            iotHubIssuedCertificates = await RequestCertificateFromIotHubAsync(connectionClient, connectionContext, getIotHubCertificateSigningKey(), cancellationToken);

            await connectionClient.DisconnectAsync(cancellationToken);
        }

        if (iotHubIssuedCertificates == null)
        {
            return;
        }

        // The connection context now holds the certificate IoT Hub issued, and the authentication provider presents it the
        // next time the device connects to IoT Hub, whether that is an automatic reconnect or, as here, a new client that
        // the application seeds with the context (for example after restoring it from disk across a reboot).
        Console.WriteLine("Reconnecting to IoT Hub with the certificate IoT Hub issued");
        using ConnectionClient renewedConnectionClient = new(connectionContext: connectionContext);
        ConnectionContext renewedConnectionContext = await renewedConnectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cancellationToken);

        string issuer = ReferenceEquals(renewedConnectionContext.IssuedClientCertificates, iotHubIssuedCertificates) ? "IoT Hub" : "DPS (after re-provisioning)";
        Console.WriteLine($"Device {connectionContext.DeviceId} connected to {connectionContext.IotHubHostName} with the certificate {issuer} issued: {DescribeCertificateChain(renewedConnectionContext.IssuedClientCertificates!)}");

        await renewedConnectionClient.DisconnectAsync(cancellationToken);
    }

    /// <summary>
    /// Sends a certificate signing request to IoT Hub over the current connection and waits for the issued certificate.
    /// </summary>
    /// <returns>The issued certificate chain, or null if IoT Hub rejected the request.</returns>
    private static async Task<IReadOnlyList<string>?> RequestCertificateFromIotHubAsync(
        ConnectionClient connectionClient,
        ConnectionContext connectionContext,
        ECDsa privateKey,
        CancellationToken cancellationToken)
    {
        // The certificate signing request's subject must be the device Id. Replace "*" supersedes any earlier request for
        // this device that is still in progress, for example one interrupted by a lost connection.
        IotHubCertificateSigningRequest request = new(
            connectionContext.DeviceId,
            CreateCertificateSigningRequest(connectionContext.DeviceId, privateKey),
            replace: "*");

        Console.WriteLine($"Sending a certificate signing request to IoT Hub with request Id {request.RequestId}");
        CertificateSigningOperation operation = await connectionClient.SendCertificateSigningRequestAsync(request, cancellationToken);

        try
        {
            CertificateSigningRequestAccepted accepted = await operation.Accepted.WaitAsync(cancellationToken);
            Console.WriteLine($"IoT Hub accepted the certificate signing request; it expires at {accepted.OperationExpires:u}");

            CertificateSigningResponse response = await operation.Completed.WaitAsync(cancellationToken);
            Console.WriteLine($"IoT Hub issued {DescribeCertificateChain(response.Certificates)}");
            return response.Certificates;
        }
        catch (CertificateSigningRequestFailedException ex)
        {
            string retryAfter = ex.Error.RetryAfterSeconds is int seconds ? $" Retry after {seconds} seconds." : string.Empty;
            Console.WriteLine($"IoT Hub rejected the certificate signing request with error {ex.Error.ErrorCode}: {ex.Error.Message}.{retryAfter}");
            return null;
        }
    }

    private static string CreateCertificateSigningRequest(string commonName, ECDsa privateKey)
    {
        CertificateRequest request = new($"CN={commonName}", privateKey, HashAlgorithmName.SHA256);

        // DPS and IoT Hub take the DER-encoded PKCS#10 request, Base64-encoded without PEM headers
        return Convert.ToBase64String(request.CreateSigningRequest());
    }

    private static string DescribeCertificateChain(IReadOnlyList<string> certificateChain)
    {
        using X509Certificate2 deviceCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(certificateChain[0]));
        return $"'{deviceCertificate.Subject}' (thumbprint {deviceCertificate.Thumbprint}, expires {deviceCertificate.NotAfter:u}, " +
            $"{certificateChain.Count} certificate(s) in chain)";
    }

    /// <summary>
    /// Stands in for the device's secure storage of operational credentials. It owns the operational private keys and
    /// implements the certificate signing complete callback that the authentication providers invoke, before connecting
    /// to IoT Hub, once for each newly issued certificate chain.
    /// </summary>
    private sealed class OperationalCredentialStore : IDisposable
    {
        private readonly List<ECDsa> _privateKeys = new();

        public ECDsa CreateKey()
        {
            ECDsa privateKey = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            lock (_privateKeys)
            {
                _privateKeys.Add(privateKey);
            }

            return privateKey;
        }

        public Task<X509Certificate2> HandleCertificateSigningCompleteAsync(IReadOnlyList<string> issuedCertificateChain)
        {
            Console.WriteLine($"Certificate signing complete callback invoked for {DescribeCertificateChain(issuedCertificateChain)}");

            // The first certificate in the chain is the device certificate; the rest are its issuers
            using X509Certificate2 deviceCertificate = X509CertificateLoader.LoadCertificate(Convert.FromBase64String(issuedCertificateChain[0]));
            ECDsa privateKey = FindPrivateKey(deviceCertificate);

            // Persist the chain so that, after a reboot, the device could restore it into a ConnectionContext and connect to
            // IoT Hub without provisioning again. A real device would persist its private key (or HSM key handle) too.
            SampleConstants.SaveIssuedCertificateChain(ToPem(issuedCertificateChain));

            using X509Certificate2 certificateWithKey = deviceCertificate.CopyWithPrivateKey(privateKey);

            // Windows' TLS stack (SChannel) cannot present a certificate with an in-memory key, so round-trip it through
            // PKCS#12. A certificate bound to a hardware-backed key would be returned as is.
            return Task.FromResult(X509CertificateLoader.LoadPkcs12(certificateWithKey.Export(X509ContentType.Pkcs12), (string?)null));
        }

        public void Dispose()
        {
            lock (_privateKeys)
            {
                foreach (ECDsa privateKey in _privateKeys)
                {
                    privateKey.Dispose();
                }

                _privateKeys.Clear();
            }
        }

        private ECDsa FindPrivateKey(X509Certificate2 certificate)
        {
            byte[] publicKey = certificate.PublicKey.ExportSubjectPublicKeyInfo();
            lock (_privateKeys)
            {
                return _privateKeys.FirstOrDefault(k => k.ExportSubjectPublicKeyInfo().AsSpan().SequenceEqual(publicKey))
                    ?? throw new InvalidOperationException($"No private key matches the issued certificate '{certificate.Subject}'.");
            }
        }

        private static string ToPem(IReadOnlyList<string> certificateChain)
        {
            StringBuilder pem = new();
            foreach (string certificate in certificateChain)
            {
                pem.AppendLine(PemEncoding.WriteString("CERTIFICATE", Convert.FromBase64String(certificate)));
            }

            return pem.ToString();
        }
    }
}
