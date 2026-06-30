// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System.Text;
using Azure.Messaging.EventHubs.Consumer;
using Xunit;

namespace Azure.Iot.Sdk.C.E2E;

/// <summary>
/// Telemetry round-trip: the native agent provisions via DPS (x509), connects,
/// and publishes one telemetry message carrying a unique correlation id. The
/// test verifies the message arrives on the hub's EventHub-compatible endpoint.
///
/// This is the reference "fast" scenario; clone it for c2d / direct method /
/// twin. It runs in PRs (Category=Fast) and excludes the slow ADU suite.
/// </summary>
[Trait("Category", "Fast")]
public sealed class TelemetryE2ETests
{
    private static readonly TimeSpan s_overallTimeout = TimeSpan.FromMinutes(3);
    private static readonly TimeSpan s_agentTimeout = TimeSpan.FromMinutes(2);

    [Fact]
    public async Task Telemetry_DpsX509_RoundTrip()
    {
        E2ETestEnvironment.RequireDpsDeviceEnvironment();
        E2ETestEnvironment.Require(
            E2ETestEnvironment.EventHubConnectionString is not null,
            "IOTHUB_EVENTHUB_CONNECTION_STRING not set");

        string correlationId = Guid.NewGuid().ToString("N");
        string payload = $"{{\"e2e\":\"{correlationId}\"}}";

        using var overallCts = new CancellationTokenSource(s_overallTimeout);
        using DeviceContext device = E2ETestEnvironment.CreateDeviceContext();

        // Start consuming from the tail BEFORE launching the device so the
        // (slow) DPS provisioning + connect happens well after the consumer has
        // attached to every partition, eliminating the attach race.
        var found = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        Task consumerTask = ConsumeUntilFoundAsync(correlationId, found, overallCts.Token);

        // Give the EventHub reader a moment to attach to all partitions.
        await Task.Delay(TimeSpan.FromSeconds(5), overallCts.Token);

        AgentResult agent = await E2ETestEnvironment.RunAgentAsync(
            scenario: "telemetry",
            device: device,
            extraEnv: new Dictionary<string, string> { ["AZ_IOT_E2E_PAYLOAD"] = payload },
            timeout: s_agentTimeout,
            cancellationToken: overallCts.Token);

        Assert.True(
            agent.ExitCode == 0,
            $"agent exited {agent.ExitCode} (timedOut={agent.TimedOut}).\nSTDOUT:\n{agent.StdOut}\nSTDERR:\n{agent.StdErr}");

        bool observed;
        try
        {
            observed = await found.Task.WaitAsync(overallCts.Token);
        }
        catch (OperationCanceledException)
        {
            observed = false;
        }

        Assert.True(
            observed,
            $"telemetry message with correlation id {correlationId} was not observed on the EventHub endpoint within the timeout.");
    }

    private static async Task ConsumeUntilFoundAsync(
        string correlationId,
        TaskCompletionSource<bool> found,
        CancellationToken cancellationToken)
    {
        EventHubConsumerClient consumer = E2ETestEnvironment.EventHubName is { } name
            ? new EventHubConsumerClient(EventHubConsumerClient.DefaultConsumerGroupName, E2ETestEnvironment.EventHubConnectionString, name)
            : new EventHubConsumerClient(EventHubConsumerClient.DefaultConsumerGroupName, E2ETestEnvironment.EventHubConnectionString);

        try
        {
            await foreach (PartitionEvent partitionEvent in consumer.ReadEventsAsync(
                startReadingAtEarliestEvent: false, readOptions: null, cancellationToken: cancellationToken))
            {
                string body = Encoding.UTF8.GetString(partitionEvent.Data.EventBody.ToArray());
                if (body.Contains(correlationId, StringComparison.Ordinal))
                {
                    found.TrySetResult(true);
                    return;
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Overall timeout elapsed; the test assertion reports the failure.
        }
        finally
        {
            await consumer.DisposeAsync();
        }
    }
}
