// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Models;
using SetupSampleDevice;
using Microsoft.Azure.Iot.Device.Provisioning.Models;

internal class Program
{
    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromMinutes(10));

        // Cancel sample on key press
        Console.CancelKeyPress += (sender, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };
    }

    /// <summary>
    /// Rather than trying to use any saved provisioning results, just immediately provision the device
    /// </summary>
    /// <remarks>
    /// The benefits to this approach are
    ///  - No requirement to persist any previous provisioning results
    ///  - No wasting time by attempting to connect to IoT Hub using cached/persisted that may be outdated
    ///  
    /// The downsides of this approach are
    ///  - 
    /// </remarks>
    public async Task AlwaysProvisionAndConnectAsync(CancellationToken cancellationToken)
    {
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        using ConnectionClient connectionClient = new ConnectionClient();

        // Always provision the device first so that the provisioning result is up-to-date. Passing forceProvisioning
        // skips any attempt to connect directly to a cached IoT Hub assignment.
        ProvisioningSettings provisioningSettings = new(idScope);
        ConnectionContext currentConnectionContext = await connectionClient.ProvisionAndConnectAsync(
            provisioningSettings,
            authentication,
            forceProvisioning: true,
            cancellationToken: cancellationToken);

        // At this point in the sample, the device has connected to IoT Hub. Use it with your TelemetryClient/TwinClient/DirectMethodClient/etc

        await connectionClient.DisconnectAsync();
    }

    /// <summary>
    /// If your device has rebooted, but has persisted the latest provisioning result to disk, it may attempt to use those credentials to connect directly to IoT Hub.
    /// If they don't work, the device can still fallback to provisioning as needed.
    /// </summary>
    /// <remarks>
    /// The benefits to this approach are
    ///  - This approach only provisions as a last resort since provisioning may be expensive (takes time, may include certificate signing operation)
    ///  - Provisioning results may be persisted to disk so even rebooting a device does not force re-provisioning 
    /// 
    /// The downsides of this approach are
    ///  - It requires disk persistence on the device
    ///  - Persisted provisioning results may be out of date and connection client will spend some time trying to connect to IoT Hub before falling back to provisioning if necessary
    /// </remarks>
    public async Task ConnectUsingPersistedProvisioningResultAsync(CancellationToken cancellationToken)
    {
        string deviceId = SampleConstants.LoadDeviceId();
        string idScope = SampleConstants.LoadIdScope();
        string iotHubHostName = SampleConstants.LoadIotHubHostName();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        ConnectionContext persistedConnectionContext = new()
        {
            AuthenticationProvider = authentication,
            ConnectionProfile = ConnectionProfile.Classic,
            DeviceId = deviceId,
            IotHubHostName = iotHubHostName,
            IssuedClientCertificates = null,
        };

        // Provide the credentials that were persisted during device reboot to the connection client at construction.
        using ConnectionClient connectionClient = new ConnectionClient(connectionContext: persistedConnectionContext);

        ProvisioningSettings provisioningSettings = new(idScope);

        // ProvisionAndConnectAsync first attempts to connect directly to IoT Hub using the persisted connection context.
        // If that connection cannot be established (identity terminal error or the retry policy expires), it
        // automatically falls back to provisioning the device again and connecting to whichever hub it is assigned.
        ConnectionContext currentConnectionContext = await connectionClient.ProvisionAndConnectAsync(
            provisioningSettings,
            authentication,
            cancellationToken: cancellationToken);

        // At this point in the sample, the device has connected to IoT Hub and only provisioned if it was necessary. Use it with your TelemetryClient/TwinClient/DirectMethodClient/etc

        await connectionClient.DisconnectAsync();
    }

    /// <summary>
    /// If your connection client has been provisioned at least once and has not rebooted since then, you may attempt to re-connect to IoT Hub using the <see cref="ConnectionContext"/> 
    /// that is cached within the connection client.
    /// </summary>
    /// <remarks>
    /// The benefits to this approach are
    ///  - This approach only provisions as a last resort since provisioning may be expensive (takes time, may include certificate signing operation)
    ///  - No required disk persistence
    ///  
    /// The downsides of this approach are
    ///  - Provisioning results are only cached in memory, so they are lost upon device reboot
    ///  - Persisted provisioning results may be out of date and connection client will spend some time trying to connect to IoT Hub before falling back to provisioning if necessary
    /// </remarks>
    public async Task ConnectUsingCachedProvisioningResultAsync(CancellationToken cancellationToken)
    {
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        using ConnectionClient connectionClient = new ConnectionClient();

        // ProvisionAndConnectAsync first tries to connect directly to IoT Hub using any connection context cached in
        // memory from a previous provisioning run. If there is no cached context, or the cached context can no longer
        // connect (identity terminal error or the retry policy expires), it falls back to provisioning the device again.
        ProvisioningSettings provisioningSettings = new(idScope);
        ConnectionContext currentConnectionContext = await connectionClient.ProvisionAndConnectAsync(
            provisioningSettings,
            authentication,
            cancellationToken: cancellationToken);

        // At this point in the sample, the device has connected to IoT Hub and only provisioned if it was necessary. Use it with your TelemetryClient/TwinClient/DirectMethodClient/etc

        await connectionClient.DisconnectAsync();
    }
}