using MQTTnet.Protocol;

namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The ability to terminate a device's MQTT connection, which a <see cref="StubIotHubService"/> needs borrowed from
    /// elsewhere.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The stub hub is an MQTT client, not a broker, so it cannot send a DISCONNECT packet to another client's session. A
    /// real IoT hub can, because the broker in front of it is part of the service. Supplying an implementation of this
    /// interface through <see cref="StubIotHubServiceOptions.ConnectionDropper"/> gives the stub the same reach.
    /// </para>
    /// <para>
    /// <see cref="InProcessMqttBroker"/> implements this, so a test that uses the in-process broker gets connection drops
    /// for free. A test running against an external broker has to implement whatever that broker's administrative
    /// disconnect looks like.
    /// </para>
    /// </remarks>
    public interface IStubDeviceConnectionDropper
    {
        /// <summary>
        /// The device ids that currently have a live connection.
        /// </summary>
        /// <remarks>
        /// The stub uses this to pick a victim for a random drop. Returning ids that are not devices is harmless: the stub
        /// only ever drops ids it already knows to be devices it serves.
        /// </remarks>
        Task<IReadOnlyList<string>> GetConnectedDeviceIdsAsync(CancellationToken cancellationToken = default);

        /// <summary>
        /// Terminate a device's connection with the given MQTT disconnect reason code.
        /// </summary>
        /// <param name="deviceId">The device whose connection should be dropped.</param>
        /// <param name="reasonCode">The MQTT 5 DISCONNECT reason code to send.</param>
        /// <param name="reasonString">The optional human readable reason string to accompany the reason code.</param>
        /// <param name="cancellationToken">The cancellation token.</param>
        /// <returns>True if a connection was found and dropped, false if the device was not connected.</returns>
        Task<bool> DropDeviceConnectionAsync(
            string deviceId,
            MqttDisconnectReasonCode reasonCode,
            string? reasonString = null,
            CancellationToken cancellationToken = default);
    }
}
