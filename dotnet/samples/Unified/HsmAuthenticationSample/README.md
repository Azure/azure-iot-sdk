# HSM-backed X.509 authentication sample

This sample shows how to authenticate a device to Azure IoT with an X.509 certificate whose **private key never
leaves a hardware security module (HSM)**. On Linux/macOS the key is opened from a PKCS#11 token (SoftHSM2) as a
**native OpenSSL key handle**, so signing happens inside the token and the key material is never read into managed
memory.

## How it works

| File | Responsibility |
| ---- | -------------- |
| `SoftHsmKey.cs` | Opens the device private key from a PKCS#11 token (SoftHSM2) as a native `RSAOpenSsl` over `SafeEvpPKeyHandle`, using OpenSSL's PKCS#11 **provider** (or the legacy `engine_pkcs11` ENGINE). The handle only *references* the token key. Configured from the outputs of `c/eng/setup-softhsm.sh` (the same token the SDK's integration tests use). |
| `Program.cs` | Opens the token key, binds it to a device certificate via `CopyWithPrivateKey`, builds an `X509AuthenticationProvider`, and provisions + sends telemetry. |

The important detail is that `SoftHsmKey.Open()` returns a native `RSAOpenSsl`. When you bind it to an
`X509Certificate2` with `CopyWithPrivateKey`, .NET takes the `RSAOpenSsl` fast path and duplicates the key **handle**
by reference (an `EVP_PKEY_up_ref`) instead of calling `ExportParameters(true)`. The key therefore stays in the
token, and the OpenSSL PKCS#11 provider performs the handshake signature inside it.

> **Why not a managed custom `RSA`?** On every OS, `CopyWithPrivateKey` extracts the key material from a
> non-`RSAOpenSsl` key via `ExportParameters(true)` — which both defeats HSM custody and fails for a non-exportable
> token key. Only a native OpenSSL handle keeps the key in the token.

## Backing the sample with SoftHSM

`SoftHsmKey` opens a PKCS#11 token key through the OpenSSL PKCS#11 provider/engine and signs through it; the private
key never leaves the token. It is configured from the environment the C SDK's SoftHSM setup script already produces:

1. Run `c/eng/setup-softhsm.sh` and `eval` its exports. That creates the token and sets `AZ_IOT_CLIENT_KEY_URI`
   (e.g. `pkcs11:token=aziot;object=device-key;type=private?pin-source=file:/tmp/token-pin`). The signing PIN is read
   by the provider/engine from the URI's `pin-source`/`pin-value`.
2. Ensure the OpenSSL PKCS#11 provider is installed (see `c/eng/setup-pkcs11-provider.sh`), or that the legacy
   `engine_pkcs11` ENGINE is available — `SoftHsmKey` tries the provider first and falls back to the engine.
3. `dotnet run`

If `AZ_IOT_CLIENT_KEY_URI` is not set, the sample prints a message telling you to run the setup script. SoftHSM and
the OpenSSL PKCS#11 provider are a Linux/macOS construct, which is also where the native-handle binding works.

## Using a different HSM

The same shape works for any PKCS#11 token: point `AZ_IOT_CLIENT_KEY_URI` at your token's key object. For other
custody models:

- **PKCS#11** (most cross-platform HSMs, smart cards, TPMs via a PKCS#11 layer): use `SoftHsmKey` as-is with your
  token's RFC 7512 URI; the OpenSSL provider/engine handles the session and PIN.
- **Windows CNG / Key Storage Provider**: load the certificate from the certificate store and pass it straight to
  `X509AuthenticationProvider`; SChannel calls the KSP to sign (see below).
- **Azure Key Vault**: use the `RSAKeyVault` type from the Key Vault SDK, which surfaces the key as an `RSA` whose
  signing is delegated to Key Vault.

## Running the sample

The sample takes the branch appropriate to the OS:

- **Linux / macOS** opens the SoftHSM token key natively, demonstrates the in-token signing path offline, attaches
  the key to the certificate, then — if `DPS_ID_SCOPE` is set and the certificate's public key is enrolled in DPS —
  provisions and sends telemetry.
- **Windows** loads the (CNG/KSP-backed) certificate from the certificate store. Install your HSM vendor's Key
  Storage Provider and provision the certificate into the store first.

To run the provisioning + telemetry portion (Linux / macOS):

1. Enroll the device certificate's public key in your Device Provisioning Service (DPS) instance.
2. Set the `DPS_ID_SCOPE` environment variable (and optionally `SAMPLE_DEVICE_ID`).
3. `dotnet run`

### Why the platform difference?

On Windows the TLS stack (SChannel) requires the certificate's key to live in a CNG/CAPI provider, so HSM keys are
surfaced through a **CNG Key Storage Provider** and loaded from the certificate store. On Linux and macOS the
OpenSSL-based stack can drive a PKCS#11 token directly through the OpenSSL PKCS#11 provider/engine, so the key is
opened as a native handle and stays in the token.
