# Authentication sample

This sample demonstrates several of the most common ways to authenticate your device with the Device Provisioning Service (DPS) and IoT Hub. It also demonstrates using the certificate signing features in DPS and IoT Hub as part of your authentication strategy.

The SDK uses a single ```AuthenticationProvider``` abstract class to authenticate a device with both DPS and IoT Hub. You can derive from it to provide a custom authentication provider. Its ```HasCertificateSigningHandler``` property reports whether the provider was given a private key or callback that turns client certificates issued by DPS or IoT Hub into an ```X509Certificate2```.

## X509AuthenticationProvider

** This is the recommended approach **

This approach relies on defining an enrollment in DPS that uses an x509 attestation method. The device provides its device-specific x509 certificate, which is used with both DPS and IoT Hub. See the ```X509Certificate``` sample method.

## SymmetricKeyAuthenticationProvider

This approach relies on defining an enrollment in DPS that uses a symmetric key attestation method. The device provides its registration Id, the DPS ID scope and its device-specific symmetric key, which is used with both DPS and IoT Hub. See the ```SymmetricKey``` sample method.

## Using issued client certificates

Construct either provider with the device's local private key or a callback that turns issued client certificates into an ```X509Certificate2``` to use certificates issued by DPS or IoT Hub. Once a client certificate has been issued, the provider authenticates with IoT Hub using it. Until then, it falls back to the symmetric key or the original x509 certificate. See the ```SymmetricKeyWithIssuedCertificates``` and ```X509CertificateWithIssuedCertificates``` sample methods.

A provider with a certificate signing handler is required when the device sends a certificate signing request: ```ProvisionAndConnectAsync``` throws ```ArgumentException``` when given a ```ProvisioningCertificateSigningRequest``` and a provider without one, and ```SendCertificateSigningRequestAsync``` throws ```InvalidOperationException``` in the same situation. Providers without a handler ignore issued certificates.
