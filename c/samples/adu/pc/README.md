<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# ADU PC Sample — device update over DPS (simulated install)

This sample runs the **entire** Azure Device Update on-device workflow end to end
against a real service, but with **simulated** download/install hooks so it is
safe to run on a dev box or in CI — it never touches real firmware. It builds and
runs on both **Linux** and **Windows**.

The device-facing update operations are issued on the device's **DPS** session and
proxied by the service to Device Update. The device never talks to Device Update
directly and needs no Device Update credential.

| Operation | When the sample uses it |
|---|---|
| `requestOnboardingUpdates` | Before provisioning — this sample calls `az_iot_adu_client_request_onboarding_update()` |
| `requestSoftwareUpdates` | Operational (already provisioned) — not used by this sample |
| `reportUpdateStatus` | After an install attempt |

---

## What you need from the service

This sample is the **device half** only. It needs a Device Update service
environment that is already provisioned and that will offer it an update; setting
one up is the service operator's side and is not covered here.

From that environment you need four things, all of which go into the environment
variables in the next section:

| You need | Used for |
|---|---|
| A DPS **ID scope** | Identifies the provisioning service the device talks to |
| A **registration id** for the device | The device's identity in that service |
| A device **X.509 certificate and private key** | How this sample authenticates to DPS |
| A **trusted CA bundle** | Validates the service's TLS certificate |

The update offered to the device must declare `compatibility` matching what this
sample reports — manufacturer `Contoso`, model `ADU-Sim`. An update that does not
match is never offered, however it was imported.

> The PowerShell scripts under [samples/common/scripts](../../common/scripts)
> provision an older, IoT-Hub-based Device Update model that this sample does not
> talk to. The DPS, device certificate, X.509 enrollment and the `AZ_IOT_*`
> variables they produce are still usable; the Device Update account, instance and
> deployment are not.

---

## Configure the sample

The sample reads these environment variables (see
[samples/common/sample_utils.c](../../common/sample_utils.c)):

| Variable | Required | Meaning |
|---|---|---|
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration id / device id |
| `AZ_IOT_CLIENT_CERT` | yes | Path to the device certificate PEM |
| `AZ_IOT_CLIENT_KEY` | yes | Path to the device private key PEM |
| `AZ_IOT_TRUSTED_CA` | yes | Trusted CA bundle, e.g. `/etc/ssl/certs/ca-certificates.crt` |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | The provisioning endpoint to use. Unset means the SDK default, `global.azure-devices-provisioning.net`; set it when your environment uses a different one. |

```bash
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_CLIENT_CERT="$PWD/device-cert.pem"
export AZ_IOT_CLIENT_KEY="$PWD/device-key.pem"
export AZ_IOT_TRUSTED_CA='/etc/ssl/certs/ca-certificates.crt'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE        = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID = '<registration-id>'
$env:AZ_IOT_CLIENT_CERT         = "$PWD\device-cert.pem"
$env:AZ_IOT_CLIENT_KEY          = "$PWD\device-key.pem"
$env:AZ_IOT_TRUSTED_CA          = "$PWD\ca.pem"
```

> Manifest signature verification works out of the box: the sample uses
> `az_iot_adu_microsoft_root_keys()`, Microsoft's published ADU production roots
> compiled into the SDK — see [Root keys](#root-keys) below.

---

## Build and run

The sample target is `az_iot_sample_adu` (built when `AZ_IOT_WITH_PAHO=ON` and the
OpenSSL ADU crypto adapter is available — both are on by default).

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
- **OpenSSL 3.0+** development headers (`libssl-dev`) for the ADU crypto adapter.
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
  succeeds. Without it the ADU crypto adapter — and therefore this sample — is
  skipped.
- **Git** to clone the repo and let CMake fetch dependencies.

Run the commands from a *Developer PowerShell for VS 2022* (or any shell where the
MSVC environment is available).

### Build

On Linux:

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_adu

# Set the environment variables above, then run the binary:
./build/linux-gcc-debug/samples/az_iot_sample_adu
```

On Windows (the same sources, the simulation knobs work identically):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_adu

# Set the environment variables above in this shell, then run the binary:
./build/windows-msvc-debug/samples/Debug/az_iot_sample_adu.exe
```

<details>
<summary>Example: build and run inside a Docker container</summary>

```bash
# Start a container and copy in the SDK plus the device credentials.
docker run -it --name adu-sample ubuntu:24.04 bash

# --- inside the container ---
apt-get update
apt-get install -y git build-essential cmake ninja-build libssl-dev \
    ca-certificates pkg-config
update-ca-certificates   # populates /etc/ssl/certs/ca-certificates.crt

git clone https://github.com/Azure/azure-iot-sdk.git
cd azure-iot-sdk
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_adu
```

In a separate host shell, copy the device certificate and key into the build
directory so `$PWD`-relative paths resolve, then run it:

```bash
docker cp device-cert.pem adu-sample:/azure-iot-sdk/build/linux-gcc-debug/samples/
docker cp device-key.pem  adu-sample:/azure-iot-sdk/build/linux-gcc-debug/samples/

# --- back inside the container ---
cd /azure-iot-sdk/build/linux-gcc-debug/samples
# export the variables from "Configure the sample", then:
./az_iot_sample_adu
```

> The container needs outbound network access to clone the repo, fetch CMake
> dependencies (Paho MQTT, azure-sdk-for-c), and reach your DPS.

</details>

Leave it running. It asks for an onboarding update on the provisioning session,
then provisions via DPS and waits.

**A 200 response carrying no `updateMetadata` means "nothing for me on this
route" — it is not an error.** An update is only offered on the route that matches
the job type: an `OnboardingUpdate` job is served **only** on the onboarding
route, which is the one this sample uses.

---

## When an update is offered

**Have the update deployed before you start the sample.** It asks once, on the
onboarding route, before it provisions — it does not poll. A deployment created
after that check has run is not picked up; restart the sample to ask again.

If an update is waiting, the sample verifies the manifest signature, runs the
simulated download/install/apply workflow and reports the result, all on stdout.

**A response carrying no update means "nothing for me on this route" — it is not
an error.** An update is only offered on the route matching the deployment, and
this sample asks on the onboarding route.

---

## Details

### What is real vs. simulated

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

The device reports these properties (see [main.c](main.c)); an update must declare
matching `compatibility` to be offered:

- **Manufacturer:** `Contoso`
- **Model:** `ADU-Sim`
- **Installed update id:** `{ provider: Contoso, name: ADU-Sim, version: 1.0.0 }`

### Root keys

ADU verifies the manifest's JWS signature against one or more RSA root public
keys. The sample calls `az_iot_adu_microsoft_root_keys()` — Microsoft's published
ADU production roots, compiled into the SDK (`src/features/adu/adu_root_keys_microsoft.c`)
— so updates imported through the real Device Update service (which signs every
manifest with Microsoft's signing service) verify with no extra setup. To accept
updates signed by your **own** root instead, build your own `az_iot_adu_root_key`
array and pass it to `az_iot_adu_client_initialize()` in place of the Microsoft
keys.

### Simulation knobs

All default off. Set them in the shell that runs the sample:

| Variable | Effect |
|---|---|
| `ADU_SIM_FAIL_STEP=<n>` | Force `install_fn` to fail at 1-based step *n* (exercises per-step result accumulation + reverse-order rollback) |
| `ADU_SIM_HASH_MISMATCH=1` | Corrupt the synthesized payload to drive the per-file hash-verification failure path |
| `ADU_SIM_REBOOT=1` | `install_fn` returns `REBOOT_REQUIRED`; the sample persists state and **exits**. Re-run it (without this knob) to `resume()` and finish the workflow |
| `ADU_SIM_DELAY_MS=<ms>` | Per-download delay so progress is observable |
| `ADU_SIM_STATE_FILE=<path>` | Resume blob path (default `./adu_sim_state.blob`) |
| `AZ_IOT_PAHO_TRACE=1` | Enable the Paho MQTT library's trace logging (`[paho-trace]` lines). Use this to diagnose `connection lost: (unknown)` — the trace reveals the underlying cause (socket error, server `DISCONNECT`, keep-alive timeout, etc.) |

```bash
# Force step 1 install to fail -> reverse-order rollback, failure reported.
ADU_SIM_FAIL_STEP=1 ./az_iot_sample_adu

# Drive a payload hash mismatch -> download verification failure.
ADU_SIM_HASH_MISMATCH=1 ./az_iot_sample_adu

# Require a reboot at install -> persist + exit; re-run to resume() and finish.
ADU_SIM_REBOOT=1 ./az_iot_sample_adu
./az_iot_sample_adu            # resumes from the persisted blob
```
