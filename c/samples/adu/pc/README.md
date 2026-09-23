<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# ADU PC Sample — Device Update over IoT Hub (simulation)

This sample runs the **entire** Azure Device Update (ADU) on-device workflow end
to end against a real IoT Hub + Device Update instance, but with **simulated**
download/install hooks so it is safe to run on a dev box or in CI — it never
touches real firmware. It builds and runs on both **Linux** and **Windows**.

If you are new to ADU, start with the **Quickstart** below: three PowerShell
scripts create the Azure resources, deploy a simulated update, and clean up.

The **Details** section further down explains what is real vs. simulated, the
simulation knobs, and how to exercise failure/rollback/reboot paths.

---

## Quickstart

Prerequisites: [Azure CLI](https://learn.microsoft.com/cli/azure/install-azure-cli)
with the IoT extension (`az extension add --name azure-iot`),
PowerShell 7+, and a logged-in subscription (`az login`).

### 1. Set up the environment

Creates a resource group, IoT Hub, DPS, Device Update account/instance, storage,
a device certificate + DPS enrollment, and sets the sample's environment
variables in your current shell:

```powershell
cd samples/common/scripts
./Initialize-AduSampleEnvironment.ps1
```

To **reuse** an existing environment provisioned by a previous run, pass its
resource group. The script discovers the IoT Hub, DPS, storage, and Device
Update account/instance inside it and reuses them (it fails if the group is
missing or incomplete, and never creates resources in this mode):

```powershell
./Initialize-AduSampleEnvironment.ps1 -ResourceGroup adu-sim-rg-72351e
```

> Manifest signature verification works out of the box: the sample uses
> `az_iot_adu_microsoft_root_keys()`, Microsoft's published ADU production roots
> compiled into the SDK — see [Root keys](#root-keys) below.

### 2. Build and run the sample

The sample target is `az_iot_sample_adu` (built when `AZ_IOT_WITH_PAHO=ON`
and the OpenSSL ADU crypto adapter is available — both are on by default).

#### Prerequisites

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

Run the commands from a *Developer PowerShell for VS 2022* (or any shell where
the MSVC environment is available).

#### Build

On a Linux box with the toolchain installed:

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_adu

# Set the env vars printed by step 1 (copy-paste the bash block it emits, or
# `source adu-sample-env.sh`), then run the binary:
./build/linux-gcc-debug/samples/az_iot_sample_adu
```

On Windows (the same sources, the simulation knobs work identically):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_adu

# Set the env vars printed by step 1 in this shell, then run the binary:
./build/windows-msvc-debug/samples/Debug/az_iot_sample_adu.exe
```

`Initialize-AduSampleEnvironment.ps1` (step 1) only configures Azure and sets the
variables in *its own* shell. When the sample runs elsewhere (a Linux host, a
container), use the bash `export` block the script prints — or the
`adu-sample-env.sh` it writes next to the generated certificate — to set the same
variables there. Copy the two PEM files (`adu-sim-device-cert.pem`,
`adu-sim-device-key.pem`) to wherever you run the binary.

<details>
<summary>Example: build and run inside a Docker container</summary>

Run step 1 on your host first (it creates the Azure resources, the device
certificate, and `adu-sample-env.sh`). Then, from another shell on the host:

```bash
# Start a container and copy in the SDK plus the generated credentials.
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

In a separate host shell, copy the credentials + env script into the container
(into the build directory so `$PWD`-relative cert paths resolve), then run it:

```bash
docker cp samples/common/scripts/adu-sim-device-cert.pem adu-sample:/azure-iot-sdk/build/linux-gcc-debug/samples/
docker cp samples/common/scripts/adu-sim-device-key.pem  adu-sample:/azure-iot-sdk/build/linux-gcc-debug/samples/
docker cp samples/common/scripts/adu-sample-env.sh       adu-sample:/azure-iot-sdk/build/linux-gcc-debug/samples/

# --- back inside the container ---
cd /azure-iot-sdk/build/linux-gcc-debug/samples
source ./adu-sample-env.sh        # or paste the export block from step 1
./az_iot_sample_adu
```

> The container needs outbound network access to clone the repo, fetch CMake
> dependencies (Paho MQTT, azure-sdk-for-c), and reach your IoT Hub / DPS.

</details>

Leave it running. It provisions via DPS, reports its device properties, and waits for a deployment.

### 3. Deploy a simulated update

In a **second** terminal (same shell session as step 1, so the env vars are
present), import a simulated update and deploy it to the running device:

```powershell
cd samples/common/scripts
./New-AduSampleDeployment.ps1
```

The update version auto-bumps to the next unused patch on each run (so every run
triggers a fresh workflow); pass `-UpdateVersion <x.y.z>` to target a specific
one. Watch the device terminal: manifest received → JWS verified → ACCEPT →
simulated download + real SHA-256 check → simulated install → reports the new
`installedUpdateId`.

### 4. Clean up

Delete everything:

```powershell
./Remove-AduSampleEnvironment.ps1
```

Or remove just the deployment + imported update and keep the infrastructure ready
for another run of step 3:

```powershell
./Remove-AduSampleEnvironment.ps1 -DeploymentOnly
```

The full cleanup is equivalent to deleting the resource group directly:

```bash
az group delete --name <resource-group> --yes
```

---

## Details

### What is real vs. simulated

| Concern | Behavior |
|---|---|
| Connection, update request/response, manifest receipt, status reporting | **Real** (Paho MQTT adapter, real provisioning/device-update endpoint) |
| Manifest JWS signature verification | **Real** (OpenSSL crypto hooks, real root keys) |
| `download_fn` | **Simulated** — synthesizes deterministic (zero-filled) payload bytes of the manifest-declared size |
| `read_file_fn` | **Simulated** — serves the same deterministic bytes back so core can run the **real** streaming SHA-256 hash check |
| `install_fn` / `apply_fn` / `backup_fn` / `restore_fn` | **Simulated** — log only; optional forced failure or reboot |
| `is_installed_fn` | Always reports "not installed" so the deployment proceeds |
| `persist_state_fn` / `load_state_fn` | Read/write the resume blob to a file so `resume()` can be exercised |

The simulated update payload is **zero-filled** on purpose: the device synthesizes
the same zero bytes the import manifest declares, so the **real** per-file SHA-256
check passes. Random content would not match.

The device reports these properties (see [main.c](main.c)); the deployment script
imports an update that matches them:

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
array and pass it to `az_iot_adu_client_initialize()` in place of the Microsoft keys.

### What the scripts do

The scripts under [scripts](scripts) wrap the Azure CLI; they print each step and
do no error handling so they stay easy to read.

| Script | Purpose |
|---|---|
| [Initialize-AduSampleEnvironment.ps1](scripts/Initialize-AduSampleEnvironment.ps1) | Create a full environment from scratch, or (with `-ResourceGroup`) discover and reuse an existing one: IoT Hub, DPS, Device Update account/instance, storage, a device cert + DPS X.509 enrollment, connection diagnostics; set the sample's env vars |
| [New-AduSampleDeployment.ps1](scripts/New-AduSampleDeployment.ps1) | Build a payload + v5 import manifest, stage+import the update, tag the device into the group, create the deployment |
| [Remove-AduSampleEnvironment.ps1](scripts/Remove-AduSampleEnvironment.ps1) | Delete the resource group (and the hub diagnostic setting), or (`-DeploymentOnly`) just the deployment + update |

`Initialize-AduSampleEnvironment.ps1` sets these environment variables in your
session — the sample reads the first group, the other two scripts read the second:

| Sample variables | Sharing variables (for the other scripts) |
|---|---|
| `AZ_IOT_DPS_ID_SCOPE`, `AZ_IOT_DPS_REGISTRATION_ID`, `AZ_IOT_CLIENT_CERT`, `AZ_IOT_CLIENT_KEY`, `AZ_IOT_TRUSTED_CA` | `AZ_IOT_ADU_RESOURCE_GROUP`, `AZ_IOT_ADU_IOTHUB`, `AZ_IOT_ADU_ACCOUNT`, `AZ_IOT_ADU_INSTANCE`, `AZ_IOT_ADU_STORAGE`, `AZ_IOT_ADU_CONTAINER`, `AZ_IOT_ADU_DEVICE_ID`, `AZ_IOT_ADU_GROUP` |

`AZ_IOT_TRUSTED_CA` defaults to the Linux system CA bundle
(`/etc/ssl/certs/ca-certificates.crt`).

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

Exercise the alternate paths, then redeploy (the deployment script always uses a
fresh deployment id):

```bash
# Force step 1 install to fail -> reverse-order rollback, failure reported.
ADU_SIM_FAIL_STEP=1 ./az_iot_sample_adu

# Drive a payload hash mismatch -> download verification failure.
ADU_SIM_HASH_MISMATCH=1 ./az_iot_sample_adu

# Require a reboot at install -> persist + exit; re-run to resume() and finish.
ADU_SIM_REBOOT=1 ./az_iot_sample_adu
./az_iot_sample_adu            # resumes from the persisted blob
```

### Tracking deployment status from the service

```bash
az iot du device deployment show \
  --account "$AZ_IOT_ADU_ACCOUNT" --instance "$AZ_IOT_ADU_INSTANCE" \
  --group-id "$AZ_IOT_ADU_GROUP" --deployment-id "$AZ_IOT_ADU_DEPLOYMENT" \
  --status -o json

az iot du device list --account "$AZ_IOT_ADU_ACCOUNT" --instance "$AZ_IOT_ADU_INSTANCE" -o table
```

### Diagnosing connection drops from the service

The init script enables an IoT Hub diagnostic setting (`adu-sim-conn-diag`) that
routes **Connections** events to a Log Analytics workspace, so server-side
disconnect reasons (e.g. a duplicate-connection eviction) can be inspected. Query
it after reproducing a drop (allow a few minutes for ingestion):

```powershell
$wsGuid = az monitor log-analytics workspace show `
  --resource-group $env:AZ_IOT_ADU_RESOURCE_GROUP `
  --workspace-name $env:AZ_IOT_ADU_LOG_WORKSPACE --query customerId -o tsv
az monitor log-analytics query -w $wsGuid --analytics-query @"
AzureDiagnostics
| where ResourceProvider == 'MICROSOFT.DEVICES' and Category == 'Connections'
| where TimeGenerated > ago(1h)
| project TimeGenerated, OperationName, ResultType, ResultDescription
| order by TimeGenerated desc
"@ -o table
```

On the device, set `AZ_IOT_PAHO_TRACE=1` to capture the client-side cause at the
same time. `Remove-AduSampleEnvironment.ps1` deletes the diagnostic setting (the
full teardown also removes the workspace with the resource group).

---

## References

- On-device design: [docs/azure-device-update.md](../../docs/azure-device-update.md)
- Protocol coverage / client API: [docs/adu-protocol-coverage.md](../../docs/adu-protocol-coverage.md)
- Azure CLI Device Update commands: <https://learn.microsoft.com/cli/azure/iot/du>
- Device Update for IoT Hub docs: <https://learn.microsoft.com/azure/iot-hub-device-update/>
