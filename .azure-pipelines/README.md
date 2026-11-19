# .NET Azure Pipelines

These are Azure DevOps Services counterparts of the .NET workflows in
`.github/workflows`. The GitHub Actions workflows are unchanged.

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
| `cleanup-e2e-resources.yml` | Age-gated cleanup of leaked .NET Azure Pipelines resources; also daily at 02:00 UTC |

On `releases/public-preview`, these pipelines cover only the classic (MQTTv3) IoT
hub: the e2e and CSR filters exclude tests with trait `Hub=MQTTv5`. MQTTv5 IoT hub
tests still run in the GitHub Actions workflows `ci-dotnet-e2e-mqttv5.yml`,
`ci-dotnet-e2e-csr.yml` and `longhaul-dotnet.yml`.

## Required configuration

1. Connect the GitHub repository to Azure Pipelines, or import it into Azure Repos.
   For GitHub, YAML `pr` triggers validate all target branches. For Azure Repos,
   configure **Build validation** branch policies instead: Azure Repos does not
   implement YAML `pr` triggers. Make the build/format/advisory/CodeQL and
   `ci-dotnet`/`ci-dotnet-e2e-csr` pipelines required as appropriate; they always
   report a gate, even for unrelated PRs, and skip their work when a PR changes
   none of their inputs. Do not add a PR path filter to these six required
   pipeline definitions: a required check that never runs blocks the PR.
   The e2e pipelines need the service connection, which fork PR builds should not
   get (see 5), so fork PRs that change their inputs cannot pass them as-is.
2. Use the Azure Resource Manager service connection named
   `aziotclb service connection`, configured with workload identity federation
   in the `azure-iot-sdks/azure-iot-sdk` project. For another project, change the
   `azureServiceConnection` parameter default in the four resource-using entry
   points. Authorize only those pipelines to use it. It must have the same
   resource-provisioning and role-assignment permissions as the GitHub workflows'
   Azure identity (IoT Hub, DPS, and ADR/certificate management for CSR).
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

The pipelines install .NET 10 explicitly and call the existing .NET build,
formatting and advisory-check commands. Relevant-change detection skips expensive
checks, provisioning and tests only on unrelated PRs; main, manual and scheduled runs execute them.
Test results appear in Azure Pipelines' Tests tab, coverage in Code Coverage,
and raw results in pipeline artifacts. Longhaul Markdown is also attached to
the run summary.

Provisioning uses the same pinned `Azure/iot-sdks-e2e-fx` PowerShell module and
configuration renderer as GitHub Actions, including the CSR-specific revision.
File upload remains disabled. The generated configuration is encrypted before
artifact publication; a random AES key is passed to the test job as a secret
output variable. Temporary plaintext configuration and CSR keys are removed
after use. Unlike GitHub Actions, Azure pipeline artifacts do not have a
per-artifact one-day retention setting.

Resource groups use `<workflow-prefix>-Ado-<project-id>-<build-id>`. Teardown
reconstructs the name without setup outputs and uses `always()` with a
15-minute cancellation grace period. The nightly janitor covers canceled agents
and asynchronous deletion failures, including CSR resources, and ignores groups
younger than eight hours. Groups without a parseable `CreatedOn` tag are stamped
on first observation and receive the shared framework's 24-hour grace period
before deletion. Its default prefixes do not touch the existing GitHub Actions
or C pipeline groups.
Queue a new run rather than retrying provisioning after teardown has deleted
that run's resources.

Azure YAML PR triggers support auto-cancellation. There is no direct equivalent
here to GitHub's cancel-in-progress grouping for main/manual runs; those runs
may overlap safely because resource groups are unique per build.
