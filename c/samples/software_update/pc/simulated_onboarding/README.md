<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# Software Updates Sample - Simulated Onboarding Install on PC

This sample runs the **entire** software updates on-device workflow end-to-end
against a real service, but with **simulated** download/install hooks so it is
safe to run on a dev box or in CI — it never touches real firmware.

## Sample features
- Target Platforms: **Linux** and **Windows**.
- Shows onboarding updates only (requestOnboardingUpdates), for a device with no
  device record yet. For a registered device, see
  [simulated_regular](../simulated_regular/README.md).
- Simulated download and install steps
- Requires Azure Device Provisioning service
- Does not require an Azure IoT Hub service

## Service Requirements
- Azure Device Provisioning service
- Azure Device Update service

### Why no IoT Hub is required?

Every device-update operation this sample uses (update check and status report)
runs on the provisioning session, before the device registers. The sample
therefore sets `dps.provision_only`: the provisioning
session is brought up and kept up, registration never runs, and the hub lifecycle
stays `Idle`. That session *is* the connection.

This is **declared, not inferred**. A device whose enrollment has no linked hub
and a device that is simply misconfigured both fail registration the same way, so
a sample that guessed from the failure would hide real misconfiguration.


## Sample Termination

It runs until interrupted (Ctrl-C), like the long-lived agent it stands in for,
and exits 0. It stops early and exits non-zero only if a lifecycle settles at
`Faulted`, or if the update check is abandoned — both of which it prints first.

> Its output is block-buffered when piped or redirected, so a run that is killed
> rather than interrupted can lose it. Prefix with `stdbuf -oL -eL` when
> capturing to a file.

---

## Configure the sample

The sample [reads](../../../common/sample_utils.c) these environment variables:

| Variable | Required | Meaning |
|---|---|---|
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration id / device id |
| `AZ_IOT_CLIENT_CERT` | yes | Path to the device certificate PEM |
| `AZ_IOT_CLIENT_KEY` | yes | Path to the device private key PEM |
| `AZ_IOT_TRUSTED_CA` | yes | Trusted CA bundle, e.g. `/etc/ssl/certs/ca-certificates.crt` |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | The provisioning endpoint to use. Unset means the SDK default, `global.azure-devices-provisioning.net`; set it when your environment uses a different one. Leaving it unset for an environment that needs it fails as a repeating `TCP/TLS connect failure` against the default host. |

**Matched against a deployed update.** These are the device's *compatibility
properties*. A value that does not match the imported update is answered
"nothing for me", which looks exactly like "nothing deployed", so the sample
prints what it reported at startup. It sends no custom compatibility
properties; configure them in [main.c](main.c) if the imported update
requires them.

| Variable | Default |
|---|---|
| `AZ_IOT_SU_MANUFACTURER` | `Contoso` |
| `AZ_IOT_SU_MODEL` | `SU-Sim` |

**Reported, not matched.** The installed update id says what is on the device
now. It takes no part in matching, and the onboarding route this sample uses
omits it entirely — changing it cannot make an update eligible.

| Variable | Default |
|---|---|
| `AZ_IOT_SU_INSTALLED_PROVIDER` | `Contoso` |
| `AZ_IOT_SU_INSTALLED_NAME` | `SU-Sim` |
| `AZ_IOT_SU_INSTALLED_VERSION` | `1.0.0` |

```bash
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_CLIENT_CERT="$PWD/device-cert.pem"
export AZ_IOT_CLIENT_KEY="$PWD/device-key.pem"
export AZ_IOT_TRUSTED_CA='/etc/ssl/certs/ca-certificates.crt'
# Only when your provisioning service is not on the default global endpoint,
# e.g. a preview environment:
# export AZ_IOT_DPS_GLOBAL_ENDPOINT='global-canary.azure-devices-provisioning.net'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE        = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID = '<registration-id>'
$env:AZ_IOT_CLIENT_CERT         = "$PWD\device-cert.pem"
$env:AZ_IOT_CLIENT_KEY          = "$PWD\device-key.pem"
$env:AZ_IOT_TRUSTED_CA          = "$PWD\ca.pem"
# Only when your provisioning service is not on the default global endpoint,
# e.g. a preview environment:
# $env:AZ_IOT_DPS_GLOBAL_ENDPOINT = 'global-canary.azure-devices-provisioning.net'
```

> Manifest signature verification works out of the box: the sample uses
> `az_iot_su_microsoft_root_keys()`, Microsoft's published software updates production roots
> compiled into the SDK — see [Root keys](#root-keys) below.

---

## Build and run

The sample target is `az_iot_sample_software_update_simulated_onboarding` (built
when `AZ_IOT_WITH_PAHO=ON` and the OpenSSL software updates crypto adapter is
available — both are on by default).

### Prerequisites

The build fetches its CMake dependencies (Paho MQTT, azure-sdk-for-c, vcpkg)
automatically, so you only need a toolchain, CMake, and OpenSSL on the host.

**Linux** (Debian/Ubuntu package names shown; adjust for your distro):

```bash
sudo apt-get update
sudo apt-get install -y \
    git build-essential cmake ninja-build \
    libssl-dev ca-certificates pkg-config
sudo update-ca-certificates   # populates /etc/ssl/certs/ca-certificates.crt
```

- A C compiler — **GCC** (`build-essential`) or **Clang**.
- **CMake 3.21+** and **Ninja** (the `linux-gcc-debug` preset uses the Ninja generator).
- **OpenSSL 3.0+** development headers (`libssl-dev`) for the software updates crypto adapter.
- **Git** to clone the repo and let CMake fetch dependencies.

**Windows:**

- **[Visual Studio 2022](https://visualstudio.microsoft.com/vs/)** with the
  *Desktop development with C++* workload (provides the MSVC compiler the
  `windows-msvc-debug` preset targets). The
  **[Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)**
  are sufficient if you don't need the IDE.
- **[CMake 3.21+](https://cmake.org/download/)** — bundled with Visual Studio, or
  install standalone and ensure `cmake` is on `PATH`.
- **OpenSSL 3.0+** — install [OpenSSL for Windows](https://slproweb.com/products/Win32OpenSSL.html)
  (or `vcpkg install openssl:x64-windows`) so CMake's `find_package(OpenSSL 3.0)`
  succeeds. Without it the software updates crypto adapter — and therefore this sample — is
  skipped.
- **Git** to clone the repo and let CMake fetch dependencies.

Run the commands from a *Developer PowerShell for VS 2022* (or any shell where the
MSVC environment is available).

### Build

Run these from the repository's `c/` directory.

On Linux:

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_software_update_simulated_onboarding

# Set the environment variables above, then run the binary:
./build/linux-gcc-debug/samples/software_update/az_iot_sample_simulated_onboarding
```

On Windows (the same sources, the simulation knobs work identically):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_software_update_simulated_onboarding

# Set the environment variables above in this shell, then run the binary:
./build/windows-msvc-debug/samples/software_update/Debug/az_iot_sample_simulated_onboarding.exe
```

<details>
<summary>Example: build and run inside a Docker container</summary>

```bash
# Start a container and copy in the SDK plus the device credentials.
docker run -it --name su-sample ubuntu:24.04 bash

# --- inside the container ---
apt-get update
apt-get install -y git build-essential cmake ninja-build libssl-dev \
    ca-certificates pkg-config
update-ca-certificates   # populates /etc/ssl/certs/ca-certificates.crt

git clone https://github.com/Azure/azure-iot-sdk.git
cd azure-iot-sdk/c
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_software_update_simulated_onboarding
```

In a separate host shell, copy the device certificate and key into the build
directory so `$PWD`-relative paths resolve, then run it:

```bash
docker cp device-cert.pem su-sample:/azure-iot-sdk/c/build/linux-gcc-debug/samples/software_update/
docker cp device-key.pem  su-sample:/azure-iot-sdk/c/build/linux-gcc-debug/samples/software_update/

# --- back inside the container ---
cd /azure-iot-sdk/c/build/linux-gcc-debug/samples/software_update
# export the variables from "Configure the sample", then:
./az_iot_sample_simulated_onboarding
```

> The container needs outbound network access to clone the repo, fetch CMake
> dependencies (Paho MQTT, azure-sdk-for-c), and reach your DPS.

</details>

Leave it running. It brings up its provisioning session, asks for an onboarding
update on that session, and waits there. It never registers — see
[Why no IoT Hub is required?](#why-no-iot-hub-is-required).

---

## When an update is offered

**Have the update deployed before you start the sample.** It asks once, on the
onboarding route, when its provisioning session comes up — it does not poll. A
deployment created after that check has run is not picked up; restart the sample
to ask again.

If an update is waiting, the sample verifies the manifest signature, runs the
simulated download/install/apply workflow and reports the result, all on stdout.

**A 200 response carrying no `updateMetadata` means "nothing for me on this
route" — it is not an error.** An update is only offered on the route that matches
the job type: an `OnboardingUpdate` job is served **only** on the onboarding
route, which is the one this sample uses.

---

## Additional Details

### What is real vs. simulated

The simulated hooks are shared with the regular-update sample, in
[../common/su_sim.c](../common/su_sim.c).

| Concern | Behavior |
|---|---|
| Connection, update request/response, manifest receipt, status reporting | **Real** (Paho MQTT adapter, real DPS endpoint) |
| Manifest JWS signature verification | **Real** (OpenSSL crypto hooks, real root keys) |
| `download_fn` | **Simulated** — synthesizes deterministic (zero-filled) payload bytes of the manifest-declared size |
| `read_file_fn` | **Simulated** — serves the same deterministic bytes back so core can run the **real** streaming SHA-256 hash check |
| `install_fn` / `apply_fn` / `backup_fn` / `restore_fn` | **Simulated** — log only; optional forced failure or reboot |
| `is_installed_fn` | Always reports "not installed" so the deployment proceeds |
| `persist_state_fn` / `load_state_fn` | Read/write the resume blob to a file so `resume()` can be exercised |

The simulated update payload is **zero-filled** on purpose: the device synthesizes
the same zero bytes the import manifest declares, so the **real** per-file SHA-256
check passes. Random content would not match.

An update must declare `compatibility` matching the first two rows to be
offered (see [main.c](main.c)):

| Property | Value | Overridden by | Matched? |
|---|---|---|---|
| Manufacturer | `Contoso` | `AZ_IOT_SU_MANUFACTURER` | yes |
| Model | `SU-Sim` | `AZ_IOT_SU_MODEL` | yes |
| Installed update id | `{ provider: Contoso, name: SU-Sim, version: 1.0.0 }` | `AZ_IOT_SU_INSTALLED_PROVIDER` / `_NAME` / `_VERSION` | no — reported only, and omitted on the onboarding route |

The sample prints what it reported at startup, so a mismatch is visible rather
than silent.

### Root keys

Software updates verifies the manifest's JWS signature against one or more RSA root public
keys. The sample calls `az_iot_su_microsoft_root_keys()` — Microsoft's published
Software updates production roots, compiled into the SDK (`src/features/su/su_root_keys_microsoft.c`)
— so manifests signed under those roots can be verified. A manifest signed
under a different key (including a preview or test issuer) fails verification;
this sample does not download roots dynamically. To trust another issuer,
validate its public key out of band, build a corresponding `az_iot_su_root_key`
array, and pass it to `az_iot_su_client_init()` instead. Do not bypass
signature verification.

### Simulation knobs

All default off. Set them in the shell that runs the sample:

| Variable | Effect |
|---|---|
| `SU_SIM_FAIL_STEP=<n>` | Force `install_fn` to fail at 1-based step *n* (exercises per-step result accumulation + reverse-order rollback) |
| `SU_SIM_HASH_MISMATCH=1` | Corrupt the synthesized payload to drive the per-file hash-verification failure path |
| `SU_SIM_REBOOT=1` | `install_fn` returns `REBOOT_REQUIRED`; the sample persists state and **exits**. Re-run it (without this knob) to `resume()` and finish the workflow |
| `SU_SIM_DELAY_MS=<ms>` | Per-download delay so progress is observable |
| `SU_SIM_STATE_FILE=<path>` | Resume blob path (default `./su_sim_state.blob`) |
| `AZ_IOT_SU_LOG_LEVEL=<lvl>` | SDK log level: `trace`, `debug`, `info` (default), `warn`, `error`, `off`. The SDK's `su:` and `dps:` protocol lines are emitted at `debug` |
| `AZ_IOT_PAHO_TRACE=1` | Enable the Paho MQTT library's trace logging (`[paho-trace]` lines). Use this to diagnose `connection lost: (unknown)` — the trace reveals the underlying cause (socket error, server `DISCONNECT`, keep-alive timeout, etc.) |

```bash
# Force step 1 install to fail -> reverse-order rollback, failure reported.
SU_SIM_FAIL_STEP=1 ./az_iot_sample_simulated_onboarding

# Drive a payload hash mismatch -> download verification failure.
SU_SIM_HASH_MISMATCH=1 ./az_iot_sample_simulated_onboarding

# Require a reboot at install -> persist + exit; re-run to resume() and finish.
SU_SIM_REBOOT=1 ./az_iot_sample_simulated_onboarding
./az_iot_sample_simulated_onboarding            # resumes from the persisted blob
```
