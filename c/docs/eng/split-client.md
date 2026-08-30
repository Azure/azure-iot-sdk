<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Splitting the SDK by IoT Hub Flavor

> **SUPERSEDED (08/10/2026) by [client-separation.md](client-separation.md).**
> This document evaluated splitting the whole SDK by **build target**
> (`AZ_IOT_FLAVOR=classic|next|universal`). That is not what was adopted, on two
> counts. A configure-time flag compiles one flavor out, so a single binary
> cannot connect and then adapt to whichever hub DPS assigned it. And the split
> turned out not to need to reach that far: the **connection client stays
> single** and keeps DPS internal, and only the **feature clients** divide by
> generation.
>
> The considerations below — migration risk, DPS duplication, feature drift,
> doubled CI — were written against the larger split. Several are reduced or
> moot at feature-client scope (DPS is never duplicated; the connection state
> machine is never forked). Read them as the risk analysis of the maximal
> version. Kept for the record.

## Abstract

There is a growing desire to split `azure-iot-sdk` into **two separate client
SDKs**, one per IoT Hub flavor:

- **Classic SDK** — talks to **Azure IoT Hub (Classic)** over **MQTT v3.1.1**,
  reusing `az::iot::hub` topic helpers from `azure-sdk-for-c`.
- **Next SDK** — talks to the new **Azure IoT/AEG Hub** over **MQTT v5**, using
  the Next protocol profile owned in this repo.

Provisioning would still go through **DPS** (MQTT v3.1.1) in both products.

This document captures the considerations of that move and, for each, the
mitigations that keep the split from regressing customer experience. The single
biggest risk is that the split **breaks the "service-side-only" hub migration
story**: today DPS can reassign a device from a Classic hub to a Next hub (via
`hub_version` in the registration result) and a single binary handles it. Two
separate SDKs turn that into a **synchronized device + service change**.

> **Context.** The current design is a *single* SDK with a runtime
> `protocol_profile` (`Classic | Next`) switch, an MQTT **adapter registry**
> keyed by MQTT version, and a DPS exchange that returns the target
> `hub_version`. See [design.md](../design.md) and
> [dps-integration.md](../dps-integration.md) for the baseline architecture.

---

## TL;DR — Considerations & Mitigations

| # | Consideration | Severity | Primary mitigation |
|---|---|:---:|---|
| 1 | Seamless hub migration (Classic ↔ Next) becomes a device + service change | 🔴 High | Ship a thin **migration shim** / "universal provisioning" package; keep DPS bootstrap shared; OTA the new binary *before* DPS reassignment |
| 2 | DPS provisioning logic must exist in **both** SDKs | 🟠 Medium | Factor DPS into a **shared `provisioning` library** consumed by both SDKs |
| 3 | Large shared core duplicated across two repos/packages | 🟠 Medium | Keep a shared **`core` library** (connection, dispatch, reconnect, cert, platform); split only the protocol-profile + feature topic layer |
| 4 | DPS must know which SDK the device runs before it can route | 🔴 High | Make `hub_version` advisory only for the *split* build; have DPS **fail closed** if the device can't honor the assigned flavor |
| 5 | Customer decision/packaging burden (which SDK do I pick?) | 🟠 Medium | Clear naming, a decision matrix, and a meta-package that pulls the right one |
| 6 | Feature drift between the two SDKs (twin, methods, ADU…) | 🟠 Medium | Shared feature-client interfaces + conformance suite run against both |
| 7 | 2× build/CI/test matrix and release cadence | 🟡 Low | Monorepo with two build targets; shared CI templates |
| 8 | Versioning & support-policy divergence | 🟡 Low | Lockstep semver for the shared core; independent minor versions per flavor |
| 9 | Documentation, samples, and support channels fork | 🟡 Low | Single docs site with per-flavor tabs; shared sample skeleton |
| 10 | Footprint win is the main upside — don't lose it | 🟢 Gain | Per-flavor builds drop the unused profile + unused MQTT adapter |

---

## 1. Seamless hub migration (the core problem)

**Consideration.** Today a single binary registers both a v3.1.1 and a v5 MQTT
factory and selects the right one from the DPS `hub_version` result. The service
can move a device from a Classic hub to a Next hub purely by re-pointing the DPS
enrollment — **no device firmware change**. After a split, a Classic-only binary
physically cannot speak MQTT v5 to a Next hub (and vice versa). Migration now
requires **both** a DPS/service change **and** an OTA firmware update that swaps
the SDK — and those two changes must be coordinated, or the device bricks its
connectivity.

This is a regression in the exact scenario the `hub_version` flag was designed to
enable (see [dps-integration.md](../dps-integration.md)).

**Mitigations.**

- **Keep a "universal" provisioning + bootstrap path shared.** Even in a split
  world, the *provisioning* code (DPS, v3.1.1) is identical. Ship it as a shared
  package so a device can always reach DPS regardless of which Hub SDK it links.
- **Offer a `migration` / `universal` meta-package** for customers who need
  in-field hub migration: it links *both* protocol profiles (effectively today's
  combined SDK). Customers who never migrate link only one flavor and keep the
  footprint win. The split becomes an *opt-in size optimization*, not a hard
  fork.
- **Sequence the migration: OTA first, then DPS reassignment.** Document the
  supported flow as (1) push firmware that contains the target flavor (or the
  universal package), (2) confirm the device is running it, (3) flip the DPS
  enrollment. Provide a device-reported capability (e.g. a twin/property
  advertising "supported hub versions") so the service can **gate reassignment**
  on device readiness.
- **Fail closed on mismatch.** If a split binary is reassigned to a hub flavor it
  cannot speak, the SDK must surface a distinct, non-fatal error
  (`AZ_IOT_ERROR_HUB_VERSION_UNSUPPORTED`) and fall back to the previous
  assignment / re-provision loop instead of hard-failing — buying time for the
  OTA to land.

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: split binary + service-only migration"]
        I1["Device runs Classic-only SDK<br/>(v3.1.1 only)"]
        I2["Service re-points DPS<br/>enrollment to Next hub"]
        I3{"Device can<br/>speak MQTT v5?"}
        I4["Connectivity lost<br/>device stranded on network"]
        I1 --> I2 --> I3
        I3 -- "No" --> I4
    end

    subgraph FIX["Solution: OTA-before-reassignment + fail-closed"]
        F1["Push firmware w/ target flavor<br/>(or universal package)"]
        F2["Device reports supported<br/>hub_versions in twin/property"]
        F3{"Service gate:<br/>device ready?"}
        F4["Flip DPS enrollment to Next"]
        F5["Device connects to Next hub"]
        F6["Stay on current hub,<br/>retry after OTA lands"]
        F1 --> F2 --> F3
        F3 -- "Yes" --> F4 --> F5
        F3 -- "No" --> F6
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant Dev as Device (Classic-only)
    participant DPS
    participant Next as IoT/AEG Hub (v5)

    Note over Dev,Next: Issue - service-only reassignment
    Dev->>DPS: REGISTER (v3.1.1)
    DPS-->>Dev: RESULT hub_version=2 (Next)
    Dev->>Next: CONNECT (MQTT v5)
    Note over Dev: binary has no v5 adapter
    Dev--xNext: cannot speak v5 - fail

    Note over Dev,Next: Solution - OTA first, gated reassignment
    Dev->>Dev: OTA to universal/Next build
    Dev->>DPS: REGISTER + supportedHubVersions=[1,2]
    DPS-->>Dev: RESULT hub_version=2 (gated on capability)
    Dev->>Next: CONNECT (MQTT v5) - success
```

---

## 2. DPS logic duplicated across both SDKs

**Consideration.** Provisioning is always MQTT v3.1.1 and is **identical**
regardless of the destination hub flavor. A naive split copies the DPS exchange,
topic build/parse, and registration-result handling into both products.

**Mitigations.**

- Factor DPS into a **standalone `provisioning` library** (wrapping
  `az::iot::provisioning`) that both Hub SDKs depend on.
- The provisioning library returns the `hub_version` and assignment details; the
  Hub SDK decides whether it *can* honor them (see consideration 1 / 4).
- This also keeps the v3.1.1 MQTT adapter requirement in one place — the Next SDK
  still needs a v3.1.1 adapter *for DPS only*, even though its Hub session is v5.

**Flow — issue vs. solution.**

```mermaid
flowchart LR
    subgraph ISSUE["Issue: duplicated DPS"]
        IC["Classic SDK<br/>DPS exchange (copy A)"]
        IN["Next SDK<br/>DPS exchange (copy B)"]
        IC -. "drift / double bugfix" .- IN
    end

    subgraph FIX["Solution: shared provisioning lib"]
        P["provisioning lib<br/>(wraps az::iot::provisioning, v3.1.1)"]
        FC["Classic SDK"]
        FN["Next SDK"]
        FC --> P
        FN --> P
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant App
    participant Prov as provisioning lib (shared, v3.1.1)
    participant DPS
    participant Hub as Hub SDK (Classic or Next)

    Note over App,Hub: Solution - one DPS path feeds either flavor
    App->>Prov: register()
    Prov->>DPS: REGISTER (MQTT v3.1.1)
    DPS-->>Prov: RESULT (fqdn, deviceId, hub_version)
    Prov-->>App: assignment + hub_version
    App->>Hub: connect(assignment)
    Note over Hub: Classic to v3.1.1 . Next to v5
```

---

## 3. Large shared core duplicated

**Consideration.** Most of the SDK is flavor-agnostic. Looking at the current
`src/core` layout — `connection_client`, `dispatch`, `reconnect`,
`certificate_provider_pem`, `mqtt_iface`, `log`, `result`, `version` — only
`protocol_profile.c` and the feature-client *topic templates* actually differ
between Classic and Next. Splitting the whole tree duplicates ~80% of the code.

**Mitigations.**

- Keep a single **`core` library** (connection lifecycle, TLS/cert config,
  dispatch table, reconnect/backoff, platform abstraction, MQTT vtable). Both
  SDKs link it.
- Split only the thin layer that genuinely diverges:
  - `protocol_profile` rows (topics, response timeouts, error-code maps).
  - Feature-client topic templates / payload schemas.
  - The MQTT-version requirement (v3.1.1 vs v5 adapter selection).
- Express the split as **two build targets over one source tree** (CMake
  options, e.g. `AZ_IOT_FLAVOR=classic|next`) rather than two repos. This is the
  cheapest way to get per-flavor binaries while keeping one place to fix bugs.

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: ~80% duplicated"]
        D1["Classic repo<br/>core + profile + features"]
        D2["Next repo<br/>core + profile + features"]
        D1 -. "duplicated maintenance" .- D2
    end

    subgraph FIX["Solution: shared core, thin split"]
        CORE["core lib<br/>connection . dispatch . reconnect .<br/>cert . mqtt_iface . platform"]
        PC["profile_classic<br/>+ feature topics (v3.1.1)"]
        PN["profile_next<br/>+ feature topics (v5)"]
        CORE --> PC
        CORE --> PN
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant Feat as Feature client (shared API)
    participant Prof as protocol_profile (per-flavor)
    participant Core as core (shared)
    participant Mqtt as MQTT adapter (v3.1.1 | v5)

    Note over Feat,Mqtt: Only the profile layer differs by flavor
    Feat->>Prof: build telemetry topic
    Prof-->>Feat: topic string
    Feat->>Core: publish(topic, payload)
    Core->>Mqtt: pub via vtable
    Mqtt-->>Core: PUBACK
    Core-->>Feat: result
```

---

## 4. DPS routing vs. device capability

**Consideration.** With one universal binary, DPS can route freely because the
device can honor either answer. With split binaries, DPS may assign a flavor the
device's binary doesn't support. DPS has no inherent knowledge of which SDK a
given device runs.

**Mitigations.**

- Have the device **advertise its supported hub version(s)** during registration
  (DPS payload / custom registration data) so DPS enrollment policy can route
  only to a compatible flavor.
- Treat `hub_version` as **advisory** for split builds: the SDK validates it
  against its own capability and, on mismatch, reports
  `HUB_VERSION_UNSUPPORTED` instead of attempting an impossible connection.
- Coordinate with the **DPS service team** on enrollment-group-level policy so
  that flavor routing is explicit and gated, not implicit. (The DPS team already
  owns how `hub_version=1` vs `=2` is decided — see
  [dps-integration.md](../dps-integration.md).)

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: blind routing"]
        A1["DPS picks hub_version<br/>(no device knowledge)"]
        A2{"Matches device<br/>binary?"}
        A3["Impossible connection"]
        A1 --> A2 -- "No" --> A3
    end

    subgraph FIX["Solution: capability-checked"]
        B1["Device sends<br/>supportedHubVersions"]
        B2["DPS policy routes to<br/>compatible flavor"]
        B3{"SDK validates<br/>hub_version"}
        B4["Connect"]
        B5["HUB_VERSION_UNSUPPORTED<br/>fail closed, re-provision"]
        B1 --> B2 --> B3
        B3 -- "supported" --> B4
        B3 -- "unsupported" --> B5
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant Dev as Device (split binary)
    participant DPS

    Note over Dev,DPS: Issue - blind routing
    Dev->>DPS: REGISTER (no capability info)
    DPS-->>Dev: hub_version=2
    Dev--xDPS: binary v3.1.1-only - cannot honor

    Note over Dev,DPS: Solution - capability-gated
    Dev->>DPS: REGISTER + supportedHubVersions=[1]
    DPS-->>Dev: hub_version=1 (gated)
    Dev->>Dev: validate - supported
    Note over Dev: on mismatch - HUB_VERSION_UNSUPPORTED, fail closed
```

---

## 5. Customer decision & packaging burden

**Consideration.** Customers now have to *choose* an SDK up front, often before
they know which hub flavor they'll be assigned. Wrong choice = rework.

**Mitigations.**

- Unambiguous package names (e.g. `azure-iot-classic`, `azure-iot-next`) plus a
  **decision matrix** in the README (footprint vs. migration flexibility vs.
  feature set).
- A **meta/umbrella package** (`azure-iot`) that, by default, pulls the universal
  build; advanced users override to a single flavor for size.
- Document the default recommendation: *if you might migrate, or you don't know
  your hub flavor, use the universal package.*

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: choose blind"]
        C1["Customer picks SDK<br/>before knowing hub flavor"]
        C2{"Right choice?"}
        C3["Rework / re-port"]
        C1 --> C2 -- "No" --> C3
    end

    subgraph FIX["Solution: meta-package + matrix"]
        M1["azure-iot meta-package"]
        M2{"Need size opt<br/>& know flavor?"}
        M3["Link single flavor<br/>(classic | next)"]
        M4["Default: universal build"]
        M1 --> M2
        M2 -- "Yes" --> M3
        M2 -- "No / unsure" --> M4
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant Cust as Customer build
    participant Pkg as azure-iot meta-package
    participant Reg as Package registry

    Note over Cust,Reg: Solution - default pulls safe (universal)
    Cust->>Pkg: depend on "azure-iot"
    Pkg->>Reg: resolve default to universal
    Reg-->>Cust: classic + next profiles
    Note over Cust: override for size
    Cust->>Pkg: AZ_IOT_FLAVOR=classic
    Pkg->>Reg: resolve to classic only
    Reg-->>Cust: v3.1.1 profile only
```

---

## 6. Feature drift between SDKs

**Consideration.** Two codebases (or two build targets maintained by different
people) drift: twin, direct methods, C2D, telemetry, and especially **ADU** can
diverge in behavior, error mapping, or API shape. ADU keeps this bounded by having
**one** channel: the twin channel (ADUv1) is cut, and the ADUv2 pull protocol,
fronted by the DPS gateway, is the only channel that will ship — it is the
implementation target, not yet built. See
[adu-client-plan.md](adu-client-plan.md). Drift would therefore be drift in the
shared engine, which is exactly what the conformance suite has to catch.

**Mitigations.**

- Define **shared feature-client interfaces** (the public `az_iot_twin_client`,
  `az_iot_direct_method_client`, etc. headers) and implement the per-flavor
  differences only behind the protocol profile.
- Run the **conformance suite** (`tests/conformance`) against *both* build
  flavors in CI, asserting identical public behavior where the protocols allow.
- Keep ADU's verify→download→install→report core shared (`adu_core`); the
  only variation is the channel implementation behind `az_iot_adu_channel`, and for now
  there is exactly one.

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: drift"]
        T1["Classic twin/methods/ADU impl"]
        T2["Next twin/methods/ADU impl"]
        T1 -. "behaviour diverges" .- T2
    end

    subgraph FIX["Solution: shared iface + conformance"]
        IF["Shared feature-client interfaces<br/>az_iot_twin_client, ..."]
        IC["Classic profile impl"]
        IN["Next profile impl"]
        CONF["conformance suite<br/>runs vs BOTH flavors in CI"]
        IF --> IC
        IF --> IN
        CONF --> IC
        CONF --> IN
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant CI
    participant Suite as conformance suite
    participant CL as Classic build
    participant NX as Next build

    Note over CI,NX: Solution - same assertions, both flavors
    CI->>Suite: run
    Suite->>CL: twin get/patch, method, telemetry
    CL-->>Suite: results
    Suite->>NX: twin get/patch, method, telemetry
    NX-->>Suite: results
    Suite-->>CI: identical public behaviour? pass/fail
```

---

## 7. Doubled build / CI / test matrix

**Consideration.** Two products mean ~2× the CI permutations (adapters, TLS
stacks, OSes) and the risk of one flavor's pipeline rotting.

**Mitigations.**

- **Monorepo, two targets.** A single repo with `AZ_IOT_FLAVOR` build option
  reuses the existing CI templates and adapter matrix; the only new axis is the
  flavor flag.
- Share CI YAML templates; parameterize the flavor. Gate PRs on both flavors
  building + the shared conformance suite passing.

**Flow — issue vs. solution.**

```mermaid
flowchart LR
    subgraph ISSUE["Issue: forked pipelines"]
        P1["Classic CI<br/>(adapters x TLS x OS)"]
        P2["Next CI<br/>(adapters x TLS x OS)"]
        P2 -. "rots / drifts" .- P1
    end

    subgraph FIX["Solution: one matrix + flavor axis"]
        Y["Shared CI template"]
        F1["flavor=classic"]
        F2["flavor=next"]
        Y --> F1
        Y --> F2
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant PR
    participant CI as Shared pipeline
    participant B1 as Build classic
    participant B2 as Build next
    participant CT as Conformance

    PR->>CI: open / update
    CI->>B1: build (flavor=classic)
    CI->>B2: build (flavor=next)
    B1-->>CI: ok
    B2-->>CI: ok
    CI->>CT: run vs both
    CT-->>CI: pass
    CI-->>PR: gate - both flavors green
```

---

## 8. Versioning & support policy

**Consideration.** Two independently versioned SDKs can develop incompatible
shared-core expectations and confusing support windows.

**Mitigations.**

- **Lockstep semver for the shared `core` and `provisioning` libraries.** Both
  flavors pin the same core version.
- Allow independent **minor** versions per flavor for flavor-specific features,
  but keep a published compatibility table.
- One support/lifecycle policy document covering both flavors.

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: divergent core pins"]
        V1["Classic vX to core 1.2"]
        V2["Next vY to core 1.5"]
        V1 -. "incompatible shared core" .- V2
    end

    subgraph FIX["Solution: lockstep core"]
        CORE["core/provisioning<br/>semver (pinned identical)"]
        FCl["Classic minor features"]
        FNx["Next minor features"]
        CORE --> FCl
        CORE --> FNx
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant Rel as Release process
    participant Core as core/provisioning
    participant CL as Classic SDK
    participant NX as Next SDK

    Note over Rel,NX: Solution - core bumps in lockstep
    Rel->>Core: tag core 1.6.0
    Core-->>CL: pin core 1.6.0
    Core-->>NX: pin core 1.6.0
    CL->>CL: classic-only minor (1.6.x)
    NX->>NX: next-only minor (1.6.x)
    Note over CL,NX: compatibility table published
```

---

## 9. Documentation, samples & support fork

**Consideration.** Docs, samples (`samples/`), and support channels tend to fork
per product, doubling maintenance and confusing users.

**Mitigations.**

- One documentation site with **per-flavor tabs/sections** rather than two sites.
- A **shared sample skeleton** (connection + provisioning) with flavor-specific
  deltas highlighted, mirroring the current `samples/common` layout.
- Single issue tracker with a `flavor:classic` / `flavor:next` label taxonomy.

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: forked content"]
        D1["Classic docs + samples"]
        D2["Next docs + samples"]
        D1 -. "double maintenance" .- D2
    end

    subgraph FIX["Solution: unified"]
        SITE["One docs site<br/>per-flavor tabs"]
        SKEL["Shared sample skeleton<br/>(connect + provisioning)"]
        dC["classic deltas"]
        dN["next deltas"]
        SITE --> SKEL
        SKEL --> dC
        SKEL --> dN
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant Dev as Developer
    participant Site as Unified docs
    participant Skel as Shared sample

    Dev->>Site: open quickstart
    Site-->>Dev: pick flavor tab (classic|next)
    Dev->>Skel: clone skeleton (connect+provision)
    Skel-->>Dev: apply flavor delta only
    Note over Dev: same core steps, minimal divergence
```

---

## 10. Footprint — the upside to preserve

**Consideration / Gain.** The legitimate motivation for splitting is **binary
size and attack surface**: a Classic-only device drops the v5 adapter and the
Next protocol profile; a Next-only device drops the v3.1.1 Hub topic tables (it
still needs v3.1.1 *for DPS*). For deeply constrained MCUs this is real flash/RAM
savings.

**Mitigations / how to keep the win without the downsides.**

- Achieve the footprint reduction via **build-time flavor selection over a shared
  tree** (consideration 3), *not* a source-level fork. You get the small binary
  and a single maintenance point.
- Use **dead-code elimination / `--gc-sections`** so the unused profile and
  unused MQTT adapter are stripped automatically when a customer links only one
  flavor — potentially delivering most of the size win even from the universal
  package, reducing the pressure to hard-split at all.

**Flow — issue vs. solution.**

```mermaid
flowchart TB
    subgraph ISSUE["Issue: carry both profiles"]
        U1["Universal binary"]
        U2["v3.1.1 + v5 adapters<br/>both profiles linked"]
        U3["Large flash/RAM on MCU"]
        U1 --> U2 --> U3
    end

    subgraph FIX["Solution: build-time select + DCE"]
        S1["AZ_IOT_FLAVOR=classic"]
        S2["--gc-sections strips<br/>v5 adapter + Next profile"]
        S3["Small binary,<br/>single source tree"]
        S1 --> S2 --> S3
    end
```

**Protocol — issue vs. solution.**

```mermaid
sequenceDiagram
    participant Build
    participant CMake
    participant Link as Linker

    Note over Build,Link: Solution - strip unused flavor at link time
    Build->>CMake: AZ_IOT_FLAVOR=classic
    CMake->>Link: compile core + classic profile
    Link->>Link: --gc-sections drop v5 adapter & Next profile
    Link-->>Build: minimal classic image
```

---

## Recommendation

Prefer a **"split by build target, not by source"** strategy:

1. One repo, one shared `core` + `provisioning`, two thin protocol-profile/feature
   layers selected by `AZ_IOT_FLAVOR`.
2. Ship three artifacts: `classic`, `next`, and a `universal` (both profiles) for
   customers who need in-field hub migration.
3. Make `hub_version` capability-checked, with the device advertising supported
   versions so DPS can gate reassignment.
4. Document the **OTA-before-reassignment** migration flow and provide a
   `HUB_VERSION_UNSUPPORTED` fail-closed path.

This captures the footprint benefit that motivates the split while preserving the
service-side migration story that the single binary gives us today.

## Version

- 06/29/2026: Created.
