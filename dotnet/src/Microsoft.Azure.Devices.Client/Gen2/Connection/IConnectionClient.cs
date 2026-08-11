using Microsoft.Azure.Devices.Client.Mqtt;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    public interface IConnectionClient : IDisposable //TODO what is the disposal pattern like with feature clients + this client? Mimic HTTP pattern of "disposing" flag?
    {
        /// <summary>
        /// Get the current connection context.
        /// </summary>
        /// <returns>Null if this connection client has not been connected yet. Otherwise, it returns the current connection context.</returns>
        public ConnectionContext? GetCurrentConnectionContext();

        public IMqttClient MqttClient { get; }
    }
}
