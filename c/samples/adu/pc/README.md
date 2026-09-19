<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# ADU PC Sample — device update over DPS (simulated install)

This sample runs the **entire** Azure Device Update on-device workflow end to end
against a real service, but with **simulated** download/install hooks so it is
safe to run on a dev box or in CI — it never touches real firmware. It builds and
runs on both **Linux** and **Windows**.

The sample implements **ADUv2**: the device-facing update operations are issued on
the device's **DPS** session and proxied **DPS → Azure Device Registry (ADR) →
Azure Device Update (ADU)**. The device never talks to ADU directly and needs no
ADU-specific credential. See
[docs/eng/aduv2-spec.md](../../../docs/eng/aduv2-spec.md) for the wire contract.

| Operation | When the sample uses it |
|---|---|
| `requestOnboardingUpdates` | Before provisioning — this sample calls `az_iot_adu_client_request_onboarding_update()` |
| `requestSoftwareUpdates` | Operational (already provisioned) — not used by this sample |
| `reportUpdateStatus` | After an install attempt |

---

## Azure environment

> **Read this before provisioning.** ADUv2 has **no Device Update "accounts"**.
> The PowerShell scripts under [samples/common/scripts](../../common/scripts)
> provision the **older, IoT-Hub-based Device Update model**
> (`az iot du account create` / `az iot du instance create --iothub-ids`), which
> is *not* the model this sample talks to. Running them does not produce an
> environment this sample can get an update from. Provision the resources below
> instead.
>
> If you have already run them, what they produce is still usable except for the
> update side: the resource group, the DPS, the device certificate and its X.509
> enrollment, the storage account, and the `AZ_IOT_DPS_ID_SCOPE`,
> `AZ_IOT_DPS_REGISTRATION_ID`, `AZ_IOT_CLIENT_CERT`, `AZ_IOT_CLIENT_KEY` and
> `AZ_IOT_TRUSTED_CA` values the sample reads. The Device Update account,
> instance and any deployment they create cannot be reused.

### Required resources

| Resource | Role |
|---|---|
| `Microsoft.DeviceUpdate/updateInstances` | Hosts and distributes update files. Replaces the account/instance pair of the older model — there is no parent account. |
| `Microsoft.DeviceRegistry/namespaces` (ADR namespace) | Device identities, groups, and deployments (`jobs` / `runs`). Linked to the update instance. |
| DPS | The device gateway. Carries the three update operations and proxies them to ADR. Needs a managed identity holding an ADR role on the namespace, and a link to that namespace. |
| Storage account + blob container | Staging for update payloads. |
| A DPS enrollment for the device | This sample authenticates to DPS with an X.509 client certificate. |

### Is an IoT Hub required?

**Not for the device-update operations.** All three are served on the DPS session
before the device registers; no hub is involved in them.

A hub is still needed for the part of this sample that runs *after* the update
check: `az_iot_connection_client_open()` provisions through DPS and then connects
to the assigned hub, so with no hub linked to the DPS, `Register` fails with
`errorCode 401001 "IoTHub not found."` (measured). Making the hub connection
optional in the sample source is being handled separately; until that lands, link
a hub to the DPS if you want the sample to get past the update check.

### Provisioning

There is currently **no script in this repo that provisions an ADUv2
environment**; the steps below are the manual equivalent.

> The api-versions, regions, ordering and failure modes recorded here were
> measured against a working environment. They have **not** been re-verified as
> part of the change that wrote this document, and the preview contract is still
> moving — read values back from your own resources rather than assuming them.

Constants used below:

```bash
ARM=https://centraluseuap.management.azure.com   # preview resources often answer
                                                 # only on the canary ARM host;
                                                 # probe management.azure.com too
API=2026-11-02-preview                           # ADR + ADU management operations
DPS_API=2026-03-01-preview                       # DPS-side ADR link property only
SUB=<subscription-id>
RG=<resource-group>
LOC=eastus2euap      # Microsoft.DeviceUpdate/updateInstances does NOT support
                     # centraluseuap (400 LocationNotAvailableForResourceType).
                     # eastus2euap is the only EUAP region supporting both
                     # updateInstances and ADR namespaces.
```

**1. Resource group**

```bash
az group create --name $RG --location $LOC
```

**2. ADU update instance**

```bash
az rest --method put \
  --url "$ARM/subscriptions/$SUB/resourceGroups/$RG/providers/Microsoft.DeviceUpdate/updateInstances/<adu-name>?api-version=$API" \
  --body "{\"location\":\"$LOC\"}"
```

Creation is slow: one instance took **~22 minutes** to reach
`provisioningState=Succeeded` (still `Creating` at 10 minutes). Budget ~40 minutes
and poll:

```bash
az rest --method get \
  --url "$ARM/subscriptions/$SUB/resourceGroups/$RG/providers/Microsoft.DeviceUpdate/updateInstances/<adu-name>?api-version=$API" \
  --query properties.provisioningState -o tsv
```

**3. ADR namespace, linked to the update instance**

Do not use `az iot adr ns create`: the pinned `azure-iot` CLI extension still
writes the retired `namespaces/credentials` shape and the service rejects it with
`DisallowedResourceOperation ... 'namespaces/credentials' is disallowed`. Use a
plain ARM REST PUT.

Wait until the update instance reports `Succeeded` first — linking a resource that
has not reached `Succeeded` fails with `LinkableResourceNotReady` ("Linked
resource provisioning state is 'Creating'").

```bash
az rest --method put \
  --url "$ARM/subscriptions/$SUB/resourceGroups/$RG/providers/Microsoft.DeviceRegistry/namespaces/<ns-name>?api-version=$API" \
  --body @namespace.json
```

`namespace.json`, the shape read back from a working environment — confirm
`endpointType` against your own service before relying on it:

```json
{
  "location": "eastus2euap",
  "identity": { "type": "SystemAssigned" },
  "properties": {
    "updating": {
      "endpoints": {
        "<endpoint-key>": {
          "endpointType": "<endpoint type>",
          "resourceId": "/subscriptions/<sub>/resourceGroups/<rg>/providers/Microsoft.DeviceUpdate/updateInstances/<adu-name>"
        }
      }
    }
  }
}
```

Notes on the link:

- One update instance links to exactly **one** ADR namespace. A second attempt is
  refused with `AduAlreadyLinked`.
- The endpoint collection is **immutable** once accepted — re-sending endpoints is
  rejected.
- A **failed** link leaves the namespace at `provisioningState: Failed`, after
  which every child write returns `409 ResourceProvisioningFailed`. Heal it with a
  **tags-only PATCH** (re-sending endpoints will not work).
- A completed link can **migrate** an endpoint between the
  `updating` / `provisioning` / `messaging` sections, so read all three when
  verifying:

  ```bash
  az rest --method get \
    --url "$ARM/subscriptions/$SUB/resourceGroups/$RG/providers/Microsoft.DeviceRegistry/namespaces/<ns-name>?api-version=$API" \
    --query "properties.[updating,provisioning,messaging]"
  ```

  Each endpoint carries `endpointType`, `resourceId`, `serviceAddress` and
  `linkingState`. `serviceAddress` is the ADU data-plane hostname and is
  **authoritative** — do not derive it from the instance name (an INT instance is
  `*.api.int.adu.microsoft.com`, production is `*.api.adu.microsoft.com`).

**4. DPS, linked to the ADR namespace**

Create the DPS, give it a managed identity, and grant that identity
**Azure Device Registry Contributor** on the namespace:

```bash
DPSID="/subscriptions/$SUB/resourceGroups/$RG/providers/Microsoft.Devices/provisioningServices/<dps-name>"
NSID="/subscriptions/$SUB/resourceGroups/$RG/providers/Microsoft.DeviceRegistry/namespaces/<ns-name>"

az iot dps create --name <dps-name> --resource-group $RG --location $LOC
az identity create --name <dps-name>-identity --resource-group $RG --location $LOC

IDID=$(az identity show --name <dps-name>-identity --resource-group $RG --query id -o tsv)
IDPRINC=$(az identity show --name <dps-name>-identity --resource-group $RG --query principalId -o tsv)

az role assignment create \
  --assignee-object-id "$IDPRINC" --assignee-principal-type ServicePrincipal \
  --role "Azure Device Registry Contributor" --scope "$NSID"
```

The identity must then be **attached to the DPS**, and the DPS's
`properties.deviceRegistryNamespace` set to the namespace resource id — a role
assignment alone gives the DPS no principal to call the registry with. Both go in
one PATCH. `deviceRegistryNamespace` exists **only at api-version
`2026-03-01-preview`**; the stable api-version silently omits it, on write and on
read-back:

```bash
az rest --method patch --url "$ARM$DPSID?api-version=$DPS_API" --body "{
  \"identity\": {
    \"type\": \"UserAssigned\",
    \"userAssignedIdentities\": { \"$IDID\": {} }
  },
  \"properties\": { \"deviceRegistryNamespace\": \"$NSID\" }
}"
```

Verify both landed — reading at the stable api-version will not show the link:

```bash
az rest --method get --url "$ARM$DPSID?api-version=$DPS_API" \
  --query "{identity:identity.type, ns:properties.deviceRegistryNamespace}"
```

**5. Storage account + container** for update payloads:

```bash
az storage account create --name <storage-name> --resource-group $RG \
  --location $LOC --sku Standard_LRS
az storage container create --account-name <storage-name> --name adu-imports \
  --auth-mode login
```

**6. Device certificate + DPS enrollment.** The sample presents an X.509 client
certificate, so create one and enroll it:

```bash
openssl req -new -x509 -days 365 -newkey rsa:2048 -nodes \
  -subj "/CN=<registration-id>" -keyout device-key.pem -out device-cert.pem

az iot dps enrollment create --dps-name <dps-name> --resource-group $RG \
  --enrollment-id <registration-id> --attestation-type x509 \
  --certificate-path device-cert.pem
```

> The ADUv2 design phases X.509 first on the update path, but X.509 there is not
> yet confirmed by measurement; SAS (enrollment-group symmetric key) auth is.

### Cleanup

Deleting the resource group removes everything:

```bash
az group delete --name <resource-group> --yes
```

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
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | see note | Overrides the SDK's global DPS endpoint. Leave it unset to use the SDK default, `global.azure-devices-provisioning.net`. **A preview/canary environment is not reachable there** — set it to `global-canary.azure-devices-provisioning.net` for an environment provisioned as above. |
| `AZ_IOT_DEVICE_ID`, `AZ_IOT_HUB_NEXT_MOCK_ENDPOINT` | no | Loaded by the shared sample config for the mock-endpoint bypass; this sample does not use them |

```bash
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_CLIENT_CERT="$PWD/device-cert.pem"
export AZ_IOT_CLIENT_KEY="$PWD/device-key.pem"
export AZ_IOT_TRUSTED_CA='/etc/ssl/certs/ca-certificates.crt'
# Preview/canary environment; omit for a production DPS.
export AZ_IOT_DPS_GLOBAL_ENDPOINT='global-canary.azure-devices-provisioning.net'
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

## Offering an update to the device

Deployments in ADUv2 are **ADR jobs and runs** on the namespace, not
`az iot du device deployment ...` — those CLI commands are account-scoped and
belong to the older model.

Create the job:

```bash
NSID="/subscriptions/$SUB/resourceGroups/$RG/providers/Microsoft.DeviceRegistry/namespaces/<ns-name>"

az rest --method put --url "$ARM$NSID/jobs/<job-name>?api-version=$API" --body '{
  "location": "eastus2euap",
  "properties": {
    "description": "ADU PC sample",
    "jobType": "OnboardingUpdate",
    "definition": {
      "schedulingType": "Continuous",
      "updateResourceId": "updates/providers/<provider>/names/<name>/versions/<version>"
    }
  }
}'
```

Then start a run:

```bash
az rest --method put --url "$ARM$NSID/jobs/<job-name>/runs/<run-name>?api-version=$API" \
  --body '{"properties":{}}'
```

Rules that bite:

- `updateResourceId` is a **relative path**, not an ARM resource id.
- `properties.definition` is **immutable**. Changing it needs delete + recreate
  (`PropertyChangeNotAllowed`).
- An `OnboardingUpdate` job is **namespace-scoped**; the service rejects a payload
  carrying a target. A `SoftwareUpdate` job instead requires
  `properties.target.resourceId = {namespace-id}/groups/<group>`.
- A `Continuous` run never reaches a terminal state, so the job cannot be deleted
  until its runs are cancelled and deleted — otherwise `409 JobHasActiveRun`.
  `POST .../runs/<run-name>/cancel` answers `202` with a `Location` header only, so
  poll the run to see it finish.
- An update whose **import** succeeded is still never offered if its
  `compatibility` does not match the device's reported `manufacturer` / `model` (this
  sample reports `Contoso` / `ADU-Sim`, see below). A successful import is not
  enough.

**Importing an update** into an ADUv2 update instance is **not documented here**:
`az iot du update init/stage/import` are account-scoped commands of the older
model and do not apply, and the ADUv2 import path has not been established by
measurement. The job above assumes an update already exists in the instance.

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

---

## References

- ADUv2 wire contract: [docs/eng/aduv2-spec.md](../../../docs/eng/aduv2-spec.md)
- On-device design: [docs/eng/adu-client-design.md](../../../docs/eng/adu-client-design.md)
- Client status / scope: [docs/eng/adu-client-plan.md](../../../docs/eng/adu-client-plan.md)
