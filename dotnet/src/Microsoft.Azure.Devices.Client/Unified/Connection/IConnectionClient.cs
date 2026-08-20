using Microsoft.Azure.Devices.Client.Mqtt;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    public interface IConnectionClient : IDisposable
    {
        /// <summary>
        /// Get the current connection context.
        /// </summary>
        /// <returns>Null if this connection client has not been connected yet. Otherwise, it returns the current connection context.</returns>
        public ConnectionContext? GetCurrentConnectionContext();

        public IMqttClient MqttClient { get; }
    }
}
