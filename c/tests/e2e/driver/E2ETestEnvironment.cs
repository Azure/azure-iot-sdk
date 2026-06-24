// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System.Diagnostics;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Xunit;

namespace Azure.Iot.Sdk.C.E2E;

/// <summary>
/// Shared environment + process helpers for the C SDK end-to-end tests.
///
/// The device side of every scenario is executed by launching the native
/// <c>az_iot_e2e_agent</c> binary (built by CMake). This class materializes the
/// X.509 material the agent needs (from the base64 env vars emitted by the
/// iot-sdks-e2e-fx config generator), prepares a CA trust bundle, locates the
/// agent binary, and runs it with a bounded timeout.
/// </summary>
internal static class E2ETestEnvironment
{
    // ---- Service / hub config (set by New-AzIotCSDKE2ETestConfig) ----------
    public static string? IotHubConnectionString => Get("IOTHUB_CONNECTION_STRING");
    public static string? EventHubConnectionString => Get("IOTHUB_EVENTHUB_CONNECTION_STRING");
    public static string? EventHubName => Get("IOTHUB_EVENTHUB_LISTEN_NAME");

    // ---- DPS x509 individual enrollment (device side) ----------------------
    public static string? DpsIdScope => Get("IOT_DPS_ID_SCOPE");
    public static string? DpsRegistrationId => Get("IOT_DPS_INDIVIDUAL_REGISTRATION_ID");
    public static string? DpsX509CertBase64 => Get("IOT_DPS_INDIVIDUAL_X509_CERTIFICATE");
    public static string? DpsX509KeyBase64 => Get("IOT_DPS_INDIVIDUAL_X509_KEY");
    public static string? DpsGlobalEndpoint => Get("IOT_DPS_GLOBAL_ENDPOINT");

    // ---- Path to the native agent (set by the e2e workflow) ----------------
    public static string? AgentPath => Get("AZ_IOT_E2E_AGENT_PATH");

    private static string? Get(string name)
    {
        string? v = Environment.GetEnvironmentVariable(name);
        return string.IsNullOrWhiteSpace(v) ? null : v;
    }

    /// <summary>
    /// Skips the calling test (instead of failing) when the cloud resources or
    /// the native agent are not available. This keeps the suite SOLID: it never
    /// produces false failures when run outside the provisioned e2e pipeline.
    /// </summary>
    public static void RequireDpsDeviceEnvironment()
    {
        Assert.SkipUnless(DpsIdScope is not null, "IOT_DPS_ID_SCOPE not set");
        Assert.SkipUnless(DpsRegistrationId is not null, "IOT_DPS_INDIVIDUAL_REGISTRATION_ID not set");
        Assert.SkipUnless(DpsX509CertBase64 is not null, "IOT_DPS_INDIVIDUAL_X509_CERTIFICATE not set");
        Assert.SkipUnless(DpsX509KeyBase64 is not null, "IOT_DPS_INDIVIDUAL_X509_KEY not set");
        Assert.SkipUnless(AgentPath is not null, "AZ_IOT_E2E_AGENT_PATH not set");
        Assert.SkipUnless(File.Exists(AgentPath), $"agent binary not found at {AgentPath}");
    }

    /// <summary>
    /// Writes the device cert/key PEMs and a CA trust bundle into a fresh temp
    /// directory and returns the environment block the native agent consumes.
    /// </summary>
    public static DeviceContext CreateDeviceContext()
    {
        string dir = Directory.CreateDirectory(
            Path.Combine(Path.GetTempPath(), "az-iot-c-e2e-" + Guid.NewGuid().ToString("N"))).FullName;

        string certPath = Path.Combine(dir, "device-cert.pem");
        string keyPath = Path.Combine(dir, "device-key.pem");
        string caPath = Path.Combine(dir, "trusted-ca.pem");

        File.WriteAllText(certPath, DecodeBase64Pem(DpsX509CertBase64!));
        File.WriteAllText(keyPath, DecodeBase64Pem(DpsX509KeyBase64!));
        File.WriteAllText(caPath, BuildTrustedCaBundle());

        var env = new Dictionary<string, string>
        {
            ["AZ_IOT_DPS_ID_SCOPE"] = DpsIdScope!,
            ["AZ_IOT_DPS_REGISTRATION_ID"] = DpsRegistrationId!,
            ["AZ_IOT_CLIENT_CERT"] = certPath,
            ["AZ_IOT_CLIENT_KEY"] = keyPath,
            ["AZ_IOT_TRUSTED_CA"] = caPath,
        };
        if (DpsGlobalEndpoint is not null)
        {
            env["AZ_IOT_DPS_GLOBAL_ENDPOINT"] = DpsGlobalEndpoint;
        }

        return new DeviceContext(dir, env);
    }

    private static string DecodeBase64Pem(string base64) =>
        Encoding.UTF8.GetString(Convert.FromBase64String(base64));

    /// <summary>
    /// Produces a PEM bundle of trusted roots for TLS server validation. On
    /// Linux the system bundle already contains the Azure roots; otherwise we
    /// export every certificate in the machine Root store. This avoids any
    /// network download (a common source of flakiness) and works on both
    /// Windows and Linux hosts.
    /// </summary>
    private static string BuildTrustedCaBundle()
    {
        const string linuxBundle = "/etc/ssl/certs/ca-certificates.crt";
        if (OperatingSystem.IsLinux() && File.Exists(linuxBundle))
        {
            return File.ReadAllText(linuxBundle);
        }

        var sb = new StringBuilder();
        foreach (StoreLocation location in new[] { StoreLocation.LocalMachine, StoreLocation.CurrentUser })
        {
            using var store = new X509Store(StoreName.Root, location);
            store.Open(OpenFlags.ReadOnly);
            foreach (X509Certificate2 cert in store.Certificates)
            {
                sb.AppendLine(cert.ExportCertificatePem());
            }
        }
        return sb.ToString();
    }

    /// <summary>
    /// Runs the native agent for a scenario and returns its exit code + output.
    /// </summary>
    public static async Task<AgentResult> RunAgentAsync(
        string scenario,
        DeviceContext device,
        IReadOnlyDictionary<string, string>? extraEnv,
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        var psi = new ProcessStartInfo
        {
            FileName = AgentPath!,
            ArgumentList = { scenario },
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (KeyValuePair<string, string> kv in device.Env)
        {
            psi.Environment[kv.Key] = kv.Value;
        }
        if (extraEnv is not null)
        {
            foreach (KeyValuePair<string, string> kv in extraEnv)
            {
                psi.Environment[kv.Key] = kv.Value;
            }
        }

        using var process = new Process { StartInfo = psi };
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        process.OutputDataReceived += (_, e) => { if (e.Data is not null) stdout.AppendLine(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) stderr.AppendLine(e.Data); };

        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(timeout);
        try
        {
            await process.WaitForExitAsync(timeoutCts.Token);
        }
        catch (OperationCanceledException)
        {
            try { process.Kill(entireProcessTree: true); } catch { /* best effort */ }
            return new AgentResult(-1, stdout.ToString(), stderr.ToString(), TimedOut: true);
        }

        return new AgentResult(process.ExitCode, stdout.ToString(), stderr.ToString(), TimedOut: false);
    }
}

/// <summary>Temp directory holding device PEM material; deleted on dispose.</summary>
internal sealed class DeviceContext(string directory, IReadOnlyDictionary<string, string> env) : IDisposable
{
    public IReadOnlyDictionary<string, string> Env { get; } = env;

    public void Dispose()
    {
        try { Directory.Delete(directory, recursive: true); } catch { /* best effort */ }
    }
}

internal sealed record AgentResult(int ExitCode, string StdOut, string StdErr, bool TimedOut);
