# Authentication sample

Shows each way a device can authenticate when it boots (with the Device Provisioning Service, DPS) and when it
operates (with IoT Hub). Each scenario where IoT Hub authenticates the device with an issued certificate then sends a
certificate signing request (CSR) to IoT Hub and reconnects with the certificate IoT Hub issues.

| Scenario | Boot (DPS) | Operational (IoT Hub) | Renewed by an IoT Hub CSR | Authentication provider |
| --- | --- | --- | --- | --- |
| `x509` | X.509 certificate | The same X.509 certificate | No | `X509AuthenticationProvider(certificate)` |
| `x509-issued` | X.509 certificate + CSR | Certificate DPS issued, paired with the CSR key by the SDK | Yes | `X509AuthenticationProvider(certificate, privateKey)` |
| `x509-issued-callback` | X.509 certificate + CSR | Certificate DPS issued, prepared by an application callback | Yes, with a new key | `X509AuthenticationProvider(certificate, callback)` |
| `sas` | SAS token from a symmetric key | SAS token from the same key | No | `SymmetricKeyAuthenticationProvider(registrationId, idScope, key)` |
| `sas-issued` | SAS token + CSR | Certificate DPS issued, paired with the CSR key by the SDK | Yes | `SymmetricKeyAuthenticationProvider(registrationId, idScope, key, privateKey)` |
| `sas-issued-callback` | SAS token + CSR | Certificate DPS issued, prepared by an application callback | Yes, with a new key | `SymmetricKeyAuthenticationProvider(registrationId, idScope, key, callback)` |

### Pairing with a private key or a callback

- **Private key**: the SDK pairs each issued certificate with the key you pass to the provider. Every CSR the device
  sends, to DPS or to IoT Hub, must be generated with that key. To rotate keys, create a new provider.
- **Callback**: the SDK invokes your callback once for each newly issued chain (Base64-encoded, device certificate
  first), before connecting to IoT Hub, and presents the certificate it returns. Use it to persist the chain, pair it
  with a key the SDK never sees (for example one in an HSM), or rotate keys. In this sample the callback saves the chain
  to `issuedcertificate.pem` and finds the private key that matches the issued certificate, so the IoT Hub CSR can use a
  new key.

After IoT Hub issues a certificate, the client's `ConnectionContext` holds it and the provider presents it the next time
the device connects to IoT Hub. The sample shows this by reconnecting with a new `ConnectionClient` seeded with that
context.

## Prerequisites

1. Run [SetupSampleDevice](../../SetupSampleDevice/) as described in the [samples README](../../README.md). It creates
   two DPS individual enrollments: an X.509 one (for the `x509*` scenarios) and a symmetric key one (for the `sas*`
   scenarios), and saves their credentials in the samples directory.
2. For the `*-issued*` scenarios only, DPS must issue certificates. That needs DPS and IoT Hub set up with
   [certificate management (preview)](https://learn.microsoft.com/azure/iot-hub/iot-hub-device-registry-setup), and the
   enrollment linked to a credential policy. Link the enrollments that SetupSampleDevice created to your policy (for
   example in the Azure portal), or point the sample at a symmetric key enrollment group that already has one (see
   below). Without a credential policy, DPS assigns the device but issues no certificate; the sample then reports that
   and stops after connecting with the boot credential.

The symmetric key credentials can also come from environment variables, all Base64-encoded where they are keys:

| Variable | Meaning |
| --- | --- |
| `SAMPLE_SYMMETRIC_KEY_REGISTRATION_ID` | Registration Id of the symmetric key device. |
| `SAMPLE_SYMMETRIC_KEY` | The device's individual enrollment key. |
| `SAMPLE_SYMMETRIC_KEY_ENROLLMENT_GROUP_KEY` | An enrollment group key; the device key is derived from it and the registration Id. Takes precedence over `SAMPLE_SYMMETRIC_KEY`. |

## Run

From the samples directory:

```bash
dotnet run --project Unified/AuthenticationSample --property WarningLevel=0 -- sas-issued
```

Run without a scenario to list them. Example output of `sas-issued-callback`:

```
Running scenario 'sas-issued-callback': As sas-issued, but the application prepares each issued certificate in a callback
Certificate signing complete callback invoked for 'CN=<registration id>' (thumbprint ..., expires ..., 2 certificate(s) in chain)
Device <device id> connected to <hub>.azure-devices.net with the certificate DPS issued: 'CN=<registration id>' (...)
Sending a certificate signing request to IoT Hub with request Id <guid>
IoT Hub accepted the certificate signing request; it expires at ...
IoT Hub issued 'CN=<device id>' (...)
Reconnecting to IoT Hub with the certificate IoT Hub issued
Certificate signing complete callback invoked for 'CN=<device id>' (...)
Device <device id> connected to <hub>.azure-devices.net with the certificate IoT Hub issued: 'CN=<device id>' (...)
```

The operational private keys in this sample live in memory and are lost when it exits. In production, keep them in
hardware (an HSM or TPM); the SDK only asks a key to sign and never exports it.
