namespace Microsoft.Azure.Devices.Client.IntegrationTests.StubService
{
    /// <summary>
    /// The generation of IoT hub that a <see cref="StubIotHubService"/> instance mimics.
    /// </summary>
    public enum IotHubGeneration
    {
        /// <summary>
        /// The "classic" IoT hub. It speaks MQTT 3.1.1, uses the <c>$iothub/...</c> and <c>devices/...</c> topic space,
        /// correlates request/response pairs with a <c>$rid</c> query parameter embedded in the topic string, and carries
        /// JSON payloads.
        /// </summary>
        Gen1,

        /// <summary>
        /// The IoT hub built on the Azure Event Grid MQTT broker. It speaks MQTT 5, uses the <c>ih/{deviceId}/{dir}/{feature}</c>
        /// topic space, correlates request/response pairs with MQTT 5 correlation data, dispatches on a <c>type</c> user property,
        /// and carries protobuf payloads.
        /// </summary>
        Gen2,
    }
}
