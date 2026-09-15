# AEG / MQTT5 hub provisioning probe

Answers, against a real subscription, the questions that decide how a **gen2 (AEG / MQTT v5)** e2e
leg gets provisioned. This is a **probe, not a gate** — delete it once the answers are folded into
`.github/actions/provision-e2e-resources`.

## What an MQTT5 hub actually is

It is **not** a SKU. It is an ordinary **S1** hub carrying:

```json
"properties": { "connectionProfile": "mqttv5" }
```

Neither the portal nor `az iot hub create` can set that yet, so the hub is created with a raw ARM
**PUT** using [`hub-body.json`](hub-body.json), on control-plane api-version `2026-08-01-preview`,
against the **global** `management.azure.com` host (the regional canary host can fail intermittently
with SSL/EOF).

Do not confuse this with the `GEN2` **SKU** on api-version `2025-08-01-preview`, which is the Azure
Device Registry / certificate-management feature and is unrelated to MQTT v5.

Creating the hub with the flag is **not proof** that it is AEG-backed. The device endpoint must
resolve through the hub's own AEG namespace:

```
<hub>.device.azure-devices.net  ->  iothub-egns-<...>.<region>-1.ts.eventgrid.azure.net
```

No `ts.eventgrid.azure.net` alias means the hub is not AEG-backed.

## Running it

### In CI (uses the pipeline's own OIDC identity)

Actions → **probe-aeg-mqtt5** → *Run workflow*. Manual dispatch only; it creates billable resources.
The report and full transcript are uploaded as the `aeg-probe-report` artifact, and a summary table
is written to the run summary.

### Locally

```powershell
az login
pwsh ./eng/aeg/probe-aeg-mqtt5.ps1 -SubscriptionId <sub>            # full run, self-cleaning
pwsh ./eng/aeg/probe-aeg-mqtt5.ps1 -SubscriptionId <sub> -WhatIfOnly     # read-only steps only
pwsh ./eng/aeg/probe-aeg-mqtt5.ps1 -SubscriptionId <sub> -KeepResources  # keep the hub for SDK testing
```

Requires **Contributor** on the subscription. Writes `aeg-probe-report.json` (machine-readable) and
`aeg-probe-transcript.txt`. Neither contains a key, connection string or certificate.

## The steps

| # | Step | Mutating | Answers |
|---|---|---|---|
| 1 | Azure identity and subscription | no | are we where we think we are? |
| 2 | `Microsoft.Devices` provider + api-version | no | is `2026-08-01-preview` offered? (warning only — the manifest is not authoritative for canary) |
| 3 | Hub name availability | no | free name |
| 4 | Create the resource group | **yes** | is the region enabled for this subscription? |
| 5 | PUT the hub with `connectionProfile: mqttv5` | **yes** | **is an MQTT5 hub allowed here at all?** |
| 6 | Poll to `Succeeded` + `Active`, re-read the profile | **yes** | did the property stick, and in what casing? Absent ⇒ silently dropped ⇒ Classic hub |
| 7 | Resolve the `.device` endpoint | **yes** | is it genuinely AEG-backed? |
| 8 | `listkeys`, mint a cert, register an X.509 device | **yes** | can a device be registered? Is the pinned `azure-iot` extension usable against an AEG hub? |
| 9 | Create DPS, link the hub | **yes** | will DPS accept an AEG hub? (`-SkipDps` to omit) |

Steps 4-9 are gated: once a required step fails the rest report `SKIP` rather than each inventing a
different downstream error.

## Known gaps this probe does not close

- **Step 9 is only half the DPS question.** Linking is necessary but not sufficient — the device must
  also receive `connectionProfile` in the DPS ASSIGNED payload (data plane `2026-11-02-preview`).
  That still has to be checked by hand. If it is absent, `AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE`
  remains mandatory in CI.
- Whether certificate management (ADR) and `connectionProfile=mqttv5` can coexist on one hub.
- AEG is canary-only (`eastus2euap`), while the certificate-management e2e runs in `EastUS`, so an
  AEG leg would not share a region with it.

## Casing

The control-plane body writes `"mqttv5"`. A later preview of the CLI extension models the same field
as `"MqttV5"`. The probe sends the former and accepts either on read-back, reporting verbatim what
the service echoed, so the run settles it.
