# HSM-backed X.509 authentication sample

This sample shows how to authenticate a device to Azure IoT with an X.509 certificate whose **private key never
leaves a hardware security module (HSM)**. It works on Windows, Linux, and macOS because the key is used through a
custom `RSA` implementation that the .NET TLS stack calls into during the handshake — the key material is never
read out of the module.

## How it works

| File | Responsibility |
| ---- | -------------- |
| `IHardwareSecurityModule.cs` | The HSM boundary: exposes the public key and a signing operation only. |
| `SimulatedHardwareSecurityModule.cs` | A runnable stand-in for a real module. Replace it with your PKCS#11 / TPM / Key Vault implementation. |
| `HsmBackedRsa.cs` | An `RSA` that forwards every private-key (signing) operation across the HSM boundary and refuses to export the private key. |
| `Program.cs` | Binds the HSM-backed key to a device certificate via `CopyWithPrivateKey`, builds an `X509AuthenticationProvider`, and provisions + sends telemetry. |

The important, reusable piece is `HsmBackedRsa`. Once an `X509Certificate2` is bound to it with
`CopyWithPrivateKey`, the rest of the SDK is unchanged: the certificate flows into the TLS layer as usual and the
handshake signature is produced inside the HSM.

## Using a real HSM

Replace `SimulatedHardwareSecurityModule` with an implementation of `IHardwareSecurityModule` that talks to your
hardware:

- **PKCS#11** (most cross-platform HSMs, smart cards, TPMs via a PKCS#11 layer): use a library such as
  [Pkcs11Interop](https://github.com/Pkcs11Interop/Pkcs11Interop). Key generation maps to `C_GenerateKeyPair`
  with the private key marked non-extractable; `SignHash` maps to `C_Sign`.
- **Windows CNG / Key Storage Provider**: you often do not need this wrapper at all — load the certificate from
  the certificate store and pass it straight to `X509AuthenticationProvider`; SChannel calls the KSP to sign.
- **Azure Key Vault**: use the `RSAKeyVault` type from the Key Vault SDK, which already has this exact shape.

## Running the sample

The sample always demonstrates the HSM signing path offline, then takes the branch appropriate to the OS:

- **Windows** loads the (CNG/KSP-backed) certificate from the certificate store. Install your HSM vendor's Key
  Storage Provider and provision the certificate into the store first.
- **Linux / macOS** attaches the managed HSM-backed key to the certificate directly, then — if `DPS_ID_SCOPE` is
  set and the certificate's public key is enrolled in DPS — provisions and sends telemetry.

To run the provisioning + telemetry portion (Linux / macOS):

1. Enroll the device certificate's public key in your Device Provisioning Service (DPS) instance.
2. Set the `DPS_ID_SCOPE` environment variable (and optionally `SAMPLE_DEVICE_ID`).
3. `dotnet run`

### Why the platform difference?

On Windows the TLS stack (SChannel) requires the certificate's key to live in a CNG/CAPI provider, so a managed
custom `RSA` cannot back a TLS certificate and `CopyWithPrivateKey` will try to export the key. HSM keys on Windows
are therefore surfaced through a **CNG Key Storage Provider** and loaded from the certificate store. On Linux and
macOS the OpenSSL-based stack calls into the managed key object, so a custom `RSA` works directly.
