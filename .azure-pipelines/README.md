# Azure Pipelines

These are Azure DevOps Services counterparts of the .NET workflows in
`.github/workflows`, and the C end-to-end tests.

Create a separate Azure Pipeline using **Existing Azure Pipelines YAML file** for
each entry point below. All pipelines also support **Run pipeline**.

| YAML file | Work |
| --- | --- |
| `ci-dotnet-build.yml` | Release build and unit tests on Linux and Windows |
| `ci-dotnet-format.yml` | Existing formatting check, including projects outside the solution |
| `ci-dotnet-vulnerable-packages.yml` | Existing direct/transitive NuGet advisory check; also daily at 13:00 UTC |
| `codeql-dotnet.yml` | C# security-extended analysis of the library and solution samples; also Mondays at 12:00 UTC |
| `ci-dotnet.yml` | Linux/Windows end-to-end tests, excluding certificate management, file upload and longhaul; also daily at 11:00 UTC |
| `ci-dotnet-e2e-csr.yml` | Certificate-management tests on Linux/Windows, serialized; also daily at 11:00 UTC |
| `longhaul-dotnet.yml` | Manual MQTTv3 longhaul with duration, OS, success-rate and memory-growth parameters |
| `ci-c-e2e.yml` | C end-to-end tests (Linux on OpenSSL 3.0 and 4.1.0, Windows on 4.1.0, four groups each; the certificate-management (CSR) suites on Linux and Windows; the connecting samples and the CSR samples on Linux and Windows; the ESP32 sample in QEMU; merged Linux coverage and, outside pull requests, the coverage combined with the unit tests') against IoT Hubs with local authentication disabled: on pull requests the shared environments, daily at 11:00 UTC on `main` and 14:00 UTC on `releases/public-preview` its own |
| `ci-c-e2e-adu.yml` | C software updates e2e tests (Linux and Windows, each adapter) on the shared MQTTv5 environment's Device Update instance (`c/docs/eng/end-to-end-tests.md`); pull requests to `releases/public-preview`, and daily at 15:00 UTC on it. Its service connection principal needs **Key Vault Secrets User** on `sharedKeyVault`, **Device Update Administrator** on the Device Update account, **Storage Blob Data Contributor** on the update storage account and **Contributor** on the environment's resource group |
| `cleanup-e2e-resources.yml` | Age-gated cleanup of leaked Azure Pipelines e2e resources; also daily at 02:00 UTC |
| `e2e-mqttv5-adr-namespace.yml` | Keeps the shared MQTTv5 environment's ADR namespace present and linked (`c/docs/eng/end-to-end-tests.md`); every 15 minutes on `releases/public-preview`. Its service connection principal needs **Key Vault Secrets User** on `sharedKeyVault` and **Contributor** on the environment's resource group |

On `releases/public-preview`, these pipelines cover only the classic (MQTTv3) IoT
hub: the e2e and CSR filters exclude tests with trait `Hub=MQTTv5`. MQTTv5 IoT hub
tests still run in the GitHub Actions workflows `ci-dotnet-e2e-mqttv5.yml` and
`longhaul-dotnet.yml`. The MQTTv5 certificate-management cases run in no pipeline:
the shared MQTTv5 environment has no symmetric-key enrollment group for them.

## Required configuration

1. Connect the GitHub repository to Azure Pipelines, or import it into Azure Repos.
   For GitHub, YAML `pr` triggers validate all target branches. For Azure Repos,
   configure **Build validation** branch policies instead: Azure Repos does not
   implement YAML `pr` triggers. Make the build/format/advisory/CodeQL and
   `ci-dotnet`/`ci-dotnet-e2e-csr`/`ci-c-e2e` pipelines required as appropriate;
   they always report a gate, even for unrelated PRs, and skip their work when a
   PR changes none of their inputs. Do not add a PR path filter to these required
   pipeline definitions: a required check that never runs blocks the PR.
   The e2e pipelines need the service connection, which fork PR builds should not
   get (see 5), so fork PRs that change their inputs cannot pass them as-is.
2. Use the Azure Resource Manager service connection named
   `aziotclb service connection`, configured with workload identity federation
   in the `azure-iot-sdks/azure-iot-sdk` project. For another project, change the
   `azureServiceConnection` parameter default in each resource-using entry
   point. Authorize only those pipelines to use it. It must have the same
   resource-provisioning and role-assignment permissions as the GitHub workflows'
   Azure identity (IoT Hub, DPS, and ADR/certificate management for CSR).
   `ci-c-e2e` creates its hub and an Event Hubs namespace with local
   authentication disabled. It grants the service connection's principal
   **IoT Hub Data Contributor** on the hub and **Azure Event Hubs Data Receiver**
   on the Event Hub, and the hub's identity **Azure Event Hubs Data Sender**, so
   that principal also needs `Microsoft.Authorization/roleAssignments/write` on
   the test resource groups (for example Owner, User Access Administrator, or
   Role Based Access Control Administrator limited to those roles).
   For `ci-c-e2e` pull requests and its ESP32 jobs, it also needs **Key Vault
   Secrets User** on the shared environment's vault (`sharedKeyVault`, default
   `kv-aziotclb`), **IoT Hub Data Contributor** on its hub and **Azure Event Hubs
   Data Receiver** on its Event Hub (`c/docs/eng/end-to-end-tests.md`).
   The default region is `eastus2euap`, matching the existing workflows; change
   `azureLocation` if the subscription uses a different region.
3. Configure CodeQL for the source host. For **Azure Repos**, enable GitHub Code
   Security/Advanced Security for the repository; the pipeline uses native
   `AdvancedSecurity-Codeql-*` tasks and publishes alerts to Azure DevOps.
   For **GitHub**, define the secret pipeline variable `GitHubCodeScanningToken`
   with code-scanning write access to this repository (classic PAT:
   `security_events`). The pinned CodeQL CLI bundle performs analysis, retains
   SARIF as an artifact, and uploads alerts to GitHub. Fork PRs retain SARIF but
   do not run the token-authenticated upload. CodeQL licensing still applies;
   standalone CLI use does not bypass private-repository licensing.
4. Ensure the organization has Microsoft-hosted parallel-job capacity.
   Longhaul requests a 360-minute job, so the agent entitlement must permit that
   duration; Azure DevOps' free private-project hosted jobs are limited to
   60 minutes. Shorten `durationMinutes` to leave time for build/reporting if
   using a shorter job allowance.
5. Restrict resource-using pipelines and service connections to trusted code.
   Do not expose service connections or secret variables to untrusted fork PRs.
   Configure pipeline retention and access to logs/artifacts for test credentials.
   These settings, branch policies and service connections are not created by
   committing YAML.

## Execution and cleanup

The .NET pipelines install .NET 10 explicitly and call the existing .NET build,
formatting and advisory-check commands. `ci-c-e2e` builds the C e2e tests with
the existing CMake presets; OpenSSL 4.1.0 is built once per image by
`c/eng/install-openssl.sh` and cached. Relevant-change detection skips expensive
checks, provisioning and tests only on unrelated PRs; main, manual and scheduled runs execute them.
Test results appear in Azure Pipelines' Tests tab, coverage in Code Coverage,
and raw results in pipeline artifacts. Longhaul Markdown is also attached to
the run summary.

Provisioning uses the same pinned `Azure/iot-sdks-e2e-fx` PowerShell module and
configuration renderer as GitHub Actions, including the CSR-specific revision.
File upload remains disabled for .NET. `ci-c-e2e` pins a revision with
`-DisableLocalAuth`: its hub accepts no shared access keys, and each test job
passes the service connection's Microsoft Entra tokens to the tests: for
`https://iothubs.azure.net` (`IOTHUB_SERVICE_ACCESS_TOKEN`) and for
`https://eventhubs.azure.net` (`IOTHUB_EVENTHUB_ACCESS_TOKEN`). The built-in Event
Hub-compatible endpoint accepts only shared access keys, so device telemetry is
routed to an Event Hub with local authentication disabled, read with that token.
Devices and DPS keep their own credentials. The CSR e2e jobs make no hub service
calls and take no tokens. The samples jobs run on their own hub, so that their
file-upload watchers do not compete with the e2e tests'
(`scripts/run-c-samples.ps1`). On pull requests, `ci-c-e2e` skips provisioning:
each job reads the shared environment's settings from Key Vault and issues its
own device (`scripts/get-c-e2e-access.ps1`). The CSR samples, whose checks the
shared CSR environment cannot serve, always provision theirs; once Azure Device
Registration links that hub, `az iot hub` refuses to update it, so its routing
is written through Azure Resource Manager. A provisioned run's
configuration is encrypted before artifact publication; a random AES key is passed to the test job as a secret
output variable. Temporary plaintext configuration and CSR keys are removed
after use. Unlike GitHub Actions, Azure pipeline artifacts do not have a
per-artifact one-day retention setting.

Resource groups use `<workflow-prefix>-Ado-<project-id>-<build-id>`
(`templates/variables-e2e-resource-group.yml`). Teardown takes the name from the
same template, without setup outputs or a checkout, and uses `always()` with a
15-minute cancellation grace period. The nightly janitor covers canceled agents
and asynchronous deletion failures, including CSR resources, and ignores groups
younger than eight hours. Groups without a parseable `CreatedOn` tag are stamped
on first observation and receive the shared framework's 24-hour grace period
before deletion. Its default prefixes do not touch the GitHub Actions groups.
Queue a new run rather than retrying provisioning after teardown has deleted
that run's resources.

Azure YAML PR triggers support auto-cancellation. There is no direct equivalent
here to GitHub's cancel-in-progress grouping for main/manual runs; those runs
may overlap safely because resource groups are unique per build.
